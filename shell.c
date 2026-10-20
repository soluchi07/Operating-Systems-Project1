#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>

#define MAX_COMMAND_LINE_LEN 1024
#define MAX_COMMAND_LINE_ARGS 128
#define MAX_PIPE_STAGES 16

#ifndef TIMEOUT_SECONDS
#define TIMEOUT_SECONDS 10
#endif

char delimiters[] = " \t\r\n";
extern char **environ;

// One command in a pipeline, e.g. "grep foo < in.txt > out.txt".
typedef struct {
    char *argv[MAX_COMMAND_LINE_ARGS];
    int argc;
    char *infile;    // target of '<', or NULL
    char *outfile;   // target of '>' or '>>', or NULL
    bool append;     // true for '>>'
} Stage;

/* ------------------------------------------------------------------ */
/* Signals                                                             */
/* ------------------------------------------------------------------ */

// The shell itself ignores the effect of Ctrl-C: just move to a fresh line.
// (write() is async-signal-safe; printf() is not.)
static void handle_sigint(int sig) {
    (void)sig;
    char nl = '\n';
    ssize_t r = write(STDOUT_FILENO, &nl, 1);
    (void)r;
}

static void install_signal_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;  // no SA_RESTART: fgets/usleep return so we can re-prompt
    if (sigaction(SIGINT, &sa, NULL) < 0) {
        perror("sigaction() failed");
        exit(1);
    }
}

/* ------------------------------------------------------------------ */
/* Tokenizer                                                           */
/* ------------------------------------------------------------------ */

static void free_tokens(char **toks, int n) {
    for (int i = 0; i < n; i++) free(toks[i]);
}

// Splits `line` into tokens. Operators (| < > >> &) become their own tokens
// (isop[i] = true) even without surrounding spaces. Single/double quotes group
// text into one word. An unquoted word starting with '$' is replaced by the
// value of that environment variable (dropped if the variable is unset).
// Returns the token count, or -1 on a syntax error (unterminated quote).
static int tokenize(const char *line, char **toks, bool *isop, int max) {
    int n = 0;
    const char *p = line;
    char *buf = malloc(strlen(line) + 1);
    if (!buf) { perror("malloc() failed"); exit(1); }

    while (*p) {
        while (*p && strchr(delimiters, *p)) p++;
        if (!*p) break;
        if (n >= max - 1) {
            fprintf(stderr, "Too many arguments\n");
            goto fail;
        }

        if (*p == '|' || *p == '<' || *p == '&') {
            toks[n] = strndup(p, 1);
            isop[n++] = true;
            p++;
        } else if (*p == '>') {
            size_t len = (p[1] == '>') ? 2 : 1;
            toks[n] = strndup(p, len);
            isop[n++] = true;
            p += len;
        } else {
            size_t len = 0;
            bool quoted = false;
            while (*p && !strchr(delimiters, *p) && !strchr("|<>&", *p)) {
                if (*p == '"' || *p == '\'') {
                    char q = *p++;
                    quoted = true;
                    while (*p && *p != q) buf[len++] = *p++;
                    if (!*p) {
                        fprintf(stderr, "Syntax error: unterminated quote\n");
                        goto fail;
                    }
                    p++;  // closing quote
                } else {
                    buf[len++] = *p++;
                }
            }
            buf[len] = '\0';

            if (!quoted && buf[0] == '$' && len > 1) {
                const char *val = getenv(buf + 1);
                if (!val) continue;  // unset variable: contributes nothing
                toks[n] = strdup(val);
            } else {
                toks[n] = strdup(buf);
            }
            isop[n++] = false;
        }
    }
    free(buf);
    return n;

fail:
    free(buf);
    free_tokens(toks, n);
    return -1;
}

/* ------------------------------------------------------------------ */
/* Parser: tokens -> pipeline of stages                                */
/* ------------------------------------------------------------------ */

// Fills `st` and returns the number of stages, or -1 on a syntax error.
// A trailing '&' sets *bg and is removed. Stage argv pointers borrow from toks.
static int parse(char **toks, const bool *isop, int n, Stage *st, bool *bg) {
    assert(n > 0);
    memset(st, 0, sizeof(Stage) * MAX_PIPE_STAGES);
    *bg = false;

    if (isop[n - 1] && strcmp(toks[n - 1], "&") == 0) {
        *bg = true;
        n--;
    }

    int cur = 0;
    for (int i = 0; i < n; i++) {
        if (!isop[i]) {
            if (st[cur].argc >= MAX_COMMAND_LINE_ARGS - 1) return -1;
            st[cur].argv[st[cur].argc++] = toks[i];
            st[cur].argv[st[cur].argc] = NULL;
        } else if (strcmp(toks[i], "|") == 0) {
            if (st[cur].argc == 0) return -1;  // nothing before the pipe
            if (++cur >= MAX_PIPE_STAGES) return -1;
        } else if (strcmp(toks[i], "<") == 0 || strcmp(toks[i], ">") == 0 ||
                   strcmp(toks[i], ">>") == 0) {
            if (i + 1 >= n || isop[i + 1]) return -1;  // missing filename
            if (toks[i][0] == '<') {
                st[cur].infile = toks[i + 1];
            } else {
                st[cur].outfile = toks[i + 1];
                st[cur].append = (toks[i][1] == '>');
            }
            i++;
        } else {
            return -1;  // '&' anywhere but the end
        }
    }
    if (st[cur].argc == 0) return -1;  // empty command (e.g. trailing '|')
    return cur + 1;
}

/* ------------------------------------------------------------------ */
/* Built-in commands                                                   */
/* ------------------------------------------------------------------ */

static bool is_builtin(const char *name) {
    static const char *names[] = {"cd", "pwd", "echo", "exit", "env", "setenv"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (strcmp(name, names[i]) == 0) return true;
    return false;
}

static void builtin_cd(int argc, char **argv) {
    const char *target = (argc > 1) ? argv[1] : getenv("HOME");
    if (!target) {
        fprintf(stderr, "cd: HOME not set\n");
        return;
    }
    if (chdir(target) < 0) {
        perror("cd failed");
        return;
    }
    char cwd[MAX_COMMAND_LINE_LEN];
    if (getcwd(cwd, sizeof(cwd))) setenv("PWD", cwd, 1);
}

static void builtin_pwd(void) {
    char cwd[MAX_COMMAND_LINE_LEN];
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        perror("getcwd() failed");
        return;
    }
    printf("%s\n", cwd);
}

static void builtin_echo(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (i > 1) putchar(' ');
        fputs(argv[i], stdout);
    }
    putchar('\n');
}

static void builtin_env(int argc, char **argv) {
    if (argc == 1) {
        for (char **e = environ; e && *e; e++) puts(*e);
    } else {
        for (int i = 1; i < argc; i++) {
            const char *v = getenv(argv[i]);
            if (v) puts(v);
        }
    }
}

// Accepts both "setenv NAME=value" and "setenv NAME value".
static void builtin_setenv(int argc, char **argv) {
    const char *name = NULL, *value = NULL;
    if (argc >= 3) {
        name = argv[1];
        value = argv[2];
    } else if (argc == 2) {
        char *eq = strchr(argv[1], '=');
        if (eq && eq != argv[1]) {
            *eq = '\0';
            name = argv[1];
            value = eq + 1;
        }
    }
    if (!name) {
        fprintf(stderr, "usage: setenv NAME=value  |  setenv NAME value\n");
        return;
    }
    if (setenv(name, value, 1) < 0) perror("setenv() failed");
}

static void run_builtin(int argc, char **argv) {
    assert(argc > 0);
    const char *cmd = argv[0];
    if (strcmp(cmd, "cd") == 0) builtin_cd(argc, argv);
    else if (strcmp(cmd, "pwd") == 0) builtin_pwd();
    else if (strcmp(cmd, "echo") == 0) builtin_echo(argc, argv);
    else if (strcmp(cmd, "env") == 0) builtin_env(argc, argv);
    else if (strcmp(cmd, "setenv") == 0) builtin_setenv(argc, argv);
    else if (strcmp(cmd, "exit") == 0) {
        fflush(stdout);
        exit(argc > 1 ? atoi(argv[1]) : 0);
    }
}

/* ------------------------------------------------------------------ */
/* Redirection and execution                                           */
/* ------------------------------------------------------------------ */

// Applies < and > to the current process's fds 0 and 1. Returns 0 or -1.
static int apply_redirs(const Stage *s) {
    if (s->infile) {
        int fd = open(s->infile, O_RDONLY);
        if (fd < 0) { perror(s->infile); return -1; }
        if (dup2(fd, STDIN_FILENO) < 0) { perror("dup2() failed"); close(fd); return -1; }
        close(fd);
    }
    if (s->outfile) {
        int flags = O_WRONLY | O_CREAT | (s->append ? O_APPEND : O_TRUNC);
        int fd = open(s->outfile, flags, 0644);
        if (fd < 0) { perror(s->outfile); return -1; }
        if (dup2(fd, STDOUT_FILENO) < 0) { perror("dup2() failed"); close(fd); return -1; }
        close(fd);
    }
    return 0;
}

// Runs a lone built-in inside the shell process (so cd/setenv/exit take
// effect), temporarily applying any redirections it has.
static void run_builtin_in_parent(Stage *s) {
    int save_in = dup(STDIN_FILENO);
    int save_out = dup(STDOUT_FILENO);
    fflush(stdout);
    if (apply_redirs(s) == 0) run_builtin(s->argc, s->argv);
    fflush(stdout);
    dup2(save_in, STDIN_FILENO);
    dup2(save_out, STDOUT_FILENO);
    close(save_in);
    close(save_out);
}

// Waits for all foreground pids. Anything still running after
// TIMEOUT_SECONDS is killed. Polls with WNOHANG so an early finish returns
// to the prompt immediately.
static void wait_foreground(const pid_t *pids, int n) {
    bool done[MAX_PIPE_STAGES] = {false};
    int remaining = n;
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);

    while (remaining > 0) {
        for (int i = 0; i < n; i++) {
            if (done[i]) continue;
            int status;
            pid_t r = waitpid(pids[i], &status, WNOHANG);
            if (r == pids[i] || (r == -1 && errno != EINTR)) {
                done[i] = true;
                remaining--;
            }
        }
        if (remaining == 0) break;

        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (now.tv_sec - start.tv_sec) +
                         (now.tv_nsec - start.tv_nsec) / 1e9;
        if (elapsed >= TIMEOUT_SECONDS) {
            fprintf(stderr, "Process timed out after %d seconds; terminating.\n",
                    TIMEOUT_SECONDS);
            for (int i = 0; i < n; i++) {
                if (done[i]) continue;
                kill(pids[i], SIGKILL);
                while (waitpid(pids[i], NULL, 0) == -1 && errno == EINTR) {}
                done[i] = true;
            }
            break;
        }
        usleep(10000);  // 10 ms; also returns early on SIGINT
    }
}

static void run_pipeline(Stage *st, int n, bool bg) {
    pid_t pids[MAX_PIPE_STAGES];
    int started = 0;
    int prev_read = -1;

    fflush(stdout);  // don't let children inherit unflushed buffered output

    for (int i = 0; i < n; i++) {
        int fd[2] = {-1, -1};
        if (i < n - 1 && pipe(fd) < 0) {
            perror("pipe() failed");
            break;
        }

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork() failed");
            if (fd[0] >= 0) { close(fd[0]); close(fd[1]); }
            break;
        }

        if (pid == 0) {
            // ----- child -----
            signal(SIGINT, SIG_DFL);
            if (bg) setpgid(0, 0);  // keep terminal Ctrl-C away from background jobs

            if (prev_read != -1) dup2(prev_read, STDIN_FILENO);
            if (fd[1] != -1) dup2(fd[1], STDOUT_FILENO);
            if (prev_read != -1) close(prev_read);
            if (fd[0] != -1) close(fd[0]);
            if (fd[1] != -1) close(fd[1]);

            // File redirections override pipe connections, like in bash.
            if (apply_redirs(&st[i]) < 0) _exit(1);

            if (is_builtin(st[i].argv[0])) {
                run_builtin(st[i].argc, st[i].argv);
                fflush(stdout);
                _exit(0);
            }

            execvp(st[i].argv[0], st[i].argv);
            perror("execvp() failed");
            fprintf(stderr, "An error occurred.\n");
            _exit(1);
        }

        // ----- parent -----
        pids[started++] = pid;
        if (bg) setpgid(pid, pid);  // also set here to avoid a race with the child
        if (prev_read != -1) close(prev_read);
        if (fd[1] != -1) close(fd[1]);
        prev_read = fd[0];
    }
    if (prev_read != -1) close(prev_read);

    if (!bg) wait_foreground(pids, started);
    // Background jobs are reaped by reap_background() before each prompt.
}

static void reap_background(void) {
    while (waitpid(-1, NULL, WNOHANG) > 0) {}
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main() {
    // Stores the string typed into the command line.
    char command_line[MAX_COMMAND_LINE_LEN];

    // Stores the tokenized command line input.
    char *arguments[MAX_COMMAND_LINE_ARGS];
    bool is_operator[MAX_COMMAND_LINE_ARGS];
    Stage stages[MAX_PIPE_STAGES];

    install_signal_handlers();

    while (true) {
        reap_background();

        // Print the shell prompt: <current working directory>>
        char cwd[MAX_COMMAND_LINE_LEN];
        if (getcwd(cwd, sizeof(cwd)) == NULL) {
            perror("getcwd() failed");
            strcpy(cwd, "?");
        }
        printf("%s>", cwd);
        fflush(stdout);

        // Read input from stdin and store it in command_line.
        if (fgets(command_line, MAX_COMMAND_LINE_LEN, stdin) == NULL) {
            // If the user input was EOF (ctrl+d), exit the shell.
            if (feof(stdin)) {
                printf("\n");
                fflush(stdout);
                fflush(stderr);
                return 0;
            }
            // Ctrl-C at the prompt interrupts fgets; just show a new prompt.
            if (errno == EINTR) {
                clearerr(stdin);
                continue;
            }
            fprintf(stderr, "fgets error");
            exit(0);
        }
        command_line[strcspn(command_line, "\n")] = '\0';

        // Tokenize the command line input.
        int ntoks = tokenize(command_line, arguments, is_operator,
                             MAX_COMMAND_LINE_ARGS);
        if (ntoks < 0) continue;   // error already reported
        if (ntoks == 0) continue;  // blank line

        // Group tokens into pipeline stages with redirections / background flag.
        bool background = false;
        int nstages = parse(arguments, is_operator, ntoks, stages, &background);
        if (nstages < 0) {
            fprintf(stderr, "Syntax error\n");
            free_tokens(arguments, ntoks);
            continue;
        }

        // Built-ins that stand alone run in the shell itself.
        if (nstages == 1 && is_builtin(stages[0].argv[0])) {
            run_builtin_in_parent(&stages[0]);
        }
        // fork, exec, and wait (unless background).
        else {
            run_pipeline(stages, nstages, background);
        }

        free_tokens(arguments, ntoks);
    }
    // This should never be reached.
    return -1;
}