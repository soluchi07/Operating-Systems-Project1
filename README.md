# Mini Shell in C: Design Report

**Author:** Soluchi Fidel-Ibeabuchi
**Course:** Operating Systems
**Assignment:** Project 1
**File:** `shell.c`

## 1. Overview

This project is a small Unix shell written in C. It prompts with the current working directory, runs a set of built-in commands, launches external programs with `fork` and `execvp`, supports background jobs, survives Ctrl-C, kills foreground jobs that run longer than 10 seconds, and supports all three kinds of I/O redirection (`<`, `>`, and `|`) for extra credit. It also accepts `>>` for appending.

The assignment is built in six stages. The code is organized so that each stage maps to a small, separate piece of `shell.c`, and the pieces connect through a simple pipeline: **read a line, tokenize it, parse it into stages, then either run a built-in or fork and exec.**

### Building and running

```
gcc -o shell shell.c -I.
./shell
```
or using the Makefile
```
make shell
./shell
```


### Example session

```
/home/codio> echo hello world
hello world
/home/codio> setenv greeting=hello
/home/codio> echo $greeting $HOME
hello /home/codio
/home/codio> cat testData | grep "this" | wc -l
2
/home/codio> cat < testData | grep last > out.txt
/home/codio> sleep 1 &
/home/codio> exit
```

## 2. Architecture

Each input line passes through four stages:

| Stage | Function | Output |
|---|---|---|
| Read | `main` loop, `fgets` | Raw string |
| Tokenize | `tokenize()` | Array of tokens plus an `isop` flag per token |
| Parse | `parse()` | Array of `Stage` structs and a background flag |
| Execute | `run_builtin_in_parent()` or `run_pipeline()` | Running processes |

Keeping tokenizing and parsing separate was the most useful structural decision. Tokenizing only decides what the words and operators are. Parsing only decides what they mean. This made it easy to add pipes and redirection late in the design without rewriting the tokenizer.

### The `Stage` structure

A pipeline such as `cat < in | grep x > out` is represented as an array of `Stage` structs, one per command:

```c
typedef struct {
    char *argv[MAX_COMMAND_LINE_ARGS]; // NULL-terminated, ready for execvp
    int argc;
    char *infile;   // target of '<', or NULL
    char *outfile;  // target of '>' or '>>', or NULL
    bool append;    // true for '>>'
} Stage;
```

Redirection operators never appear in `argv`. The parser removes them and stores the filenames in `infile` and `outfile`, so `argv` can be passed straight to `execvp`. Every command, including a plain `ls`, is treated as a pipeline of length one. That means there is one execution path for all cases instead of separate code for simple and complex commands.

## 3. Design Choices

### 3.1 Prompt (Task 1)

The prompt is built each loop iteration with `getcwd()` and printed as `<cwd>>`, matching the assignment format (for example `/home/codio>`). It is rebuilt every time because `cd` can change the directory at any point.

### 3.2 Tokenizer

I wrote a character-by-character tokenizer instead of using `strtok`, because `strtok` can only split on delimiters and cannot handle several things this shell needs:

- **Operators without spaces.** `|`, `<`, `>`, `>>`, and `&` become their own tokens even when attached to other text, so `cat file>out` works the same as `cat file > out`.
- **Quoting.** Single and double quotes group text into one token. This is needed for examples like `grep "this"`. Quotes are removed from the final token.
- **Variable expansion.** An unquoted word that starts with `$` is replaced with the value from `getenv`. If the variable is unset, the token is dropped, which matches how `echo $undefined` behaves in bash. Expanding inside the tokenizer means variables work with every command (`echo`, `cd`, `setenv`, and external programs) with no extra code, which is what the assignment hint recommends.

Tokens are copied with `strdup` and freed after each command. This keeps expanded environment values safe and avoids modifying the input buffer.

Each token also carries an `isop` flag. This matters because a quoted `"|"` should be a plain word, not a pipe. Since the flag is set only for unquoted operators, `echo "a|b"` prints `a|b` as expected.

### 3.3 Parser

The parser walks the tokens once, building stages:

- A word is appended to the current stage's `argv`.
- `|` closes the current stage and starts a new one.
- `<`, `>`, and `>>` consume the next token as a filename.
- A trailing `&` sets the background flag and is removed before parsing continues.

Malformed input is rejected with a "Syntax error" message and a fresh prompt instead of undefined behavior. Rejected cases include an empty command before or after `|`, a missing filename after a redirect, and `&` anywhere except the end of the line.

### 3.4 Built-in commands

| Command | Behavior |
|---|---|
| `cd [dir]` | Changes directory; with no argument goes to `$HOME`. Updates `PWD`. |
| `pwd` | Prints the current directory. |
| `echo args...` | Prints arguments separated by spaces. |
| `env` | Prints every entry in `environ`. |
| `env NAME` | Prints the value of that variable. |
| `setenv` | Accepts `NAME=value` and `NAME value`. |
| `exit [n]` | Exits the shell with status `n` (default 0). |

The assignment text shows `setenv greeting=hello` in its example but describes a name and a value as two arguments in its hint. I could not tell which form the tests would use, so `setenv` accepts both.

**Where built-ins run.** `cd`, `setenv`, and `exit` must change the shell's own state, so a standalone built-in runs inside the shell process rather than a child. This is why `cd` cannot be an ordinary external program. Built-ins that appear inside a pipeline, such as `echo hi | grep h`, run in a forked child so they can write into the pipe.

**Built-ins with redirection.** `echo hi > file` needs the redirection applied even though no fork occurs. For this case the shell saves stdout and stdin with `dup`, applies the redirects, runs the built-in, then restores the original descriptors.

### 3.5 Process creation (Task 2)

For anything that is not a stand-alone built-in, `run_pipeline()` forks one child per stage. Each child resets SIGINT to its default action, applies its pipe and file redirections, and calls `execvp`. If `execvp` fails, the child prints the `perror` message (`execvp() failed: No such file or directory`), prints `An error occurred.`, and exits with `_exit(1)`. It uses `_exit` rather than `exit` so the child does not flush stdio buffers it inherited from the parent.

The shell calls `fflush(stdout)` before forking. Without this, any unflushed output in the buffer would be copied into each child and printed more than once.

### 3.6 Pipes and redirection (Task 6)

For `n` stages the shell creates `n-1` pipes, one at a time as it forks. Each child connects its stdin to the previous pipe's read end and its stdout to the next pipe's write end using `dup2`, then closes every pipe descriptor it holds.

The most important detail is closing unused descriptors. A reader such as `grep` only sees end-of-file when every copy of the write end of its pipe is closed. If the parent or an earlier child kept a stray copy open, the pipeline would hang. The parent closes each pipe end as soon as the child that needs it has been forked.

File redirections are applied after pipe wiring, so they override the pipe, as in bash. For example, `ls > out | wc -l` writes to `out` and `wc` receives nothing.

### 3.7 Background processes (Task 3)

If the line ends with `&`, the parent skips waiting. Two things make this safe:

- **Zombie prevention.** Before every prompt, the shell calls `waitpid(-1, NULL, WNOHANG)` in a loop to reap any finished background children. This runs only when no foreground job is active, so it cannot steal a foreground child's exit status.
- **Separate process group.** Background children call `setpgid(0, 0)` (and the parent calls it too, to avoid a race). A Ctrl-C typed at the terminal goes to the foreground process group, so it does not kill background jobs.

### 3.8 Signal handling (Task 4)

The shell installs a SIGINT handler with `sigaction`. The handler only writes a newline, using `write()` because it is async-signal-safe and `printf` is not.

I deliberately did **not** set `SA_RESTART`. If Ctrl-C arrives while the shell is blocked in `fgets`, the call returns with `EINTR`. The main loop detects this, calls `clearerr(stdin)`, and shows a new prompt. With `SA_RESTART` the shell would keep waiting for input after Ctrl-C without redrawing the prompt. The starter code treated any `fgets` error as fatal, which would have made Ctrl-C quit the shell, so that check was reworked.

Children restore the default SIGINT action before `exec`, so Ctrl-C terminates the foreground program while the shell keeps running.

### 3.9 Timeout (Task 5)

I considered two designs: `alarm()` with a SIGALRM handler, or polling. I chose polling with `waitpid(..., WNOHANG)` because:

- It works naturally for pipelines. The shell tracks every stage's PID and kills whichever are still running, instead of trying to map one global alarm onto several children.
- It cannot affect background jobs, since only foreground PIDs are ever polled.
- It avoids a second signal handler that could interact with the SIGINT handler.

The loop checks each PID, compares elapsed time (from `CLOCK_MONOTONIC`, which is not affected by system clock changes) against `TIMEOUT_SECONDS`, and sleeps 10 ms between checks so it does not spin the CPU. If every process finishes early, the loop exits immediately and the prompt returns without waiting out the timer. After the limit, remaining processes get `SIGKILL` and are reaped with a blocking `waitpid`, so no zombie is left behind.

## 4. Code Documentation

| Function | Purpose |
|---|---|
| `handle_sigint` | SIGINT handler; writes a newline so the prompt redraws cleanly. |
| `install_signal_handlers` | Registers the handler with `sigaction` (no `SA_RESTART`). |
| `tokenize` | Splits a line into tokens, handling operators, quotes, and `$VAR`. Returns the count, or -1 on error. |
| `free_tokens` | Frees tokens created by `tokenize`. |
| `parse` | Converts tokens into `Stage` structs; sets the background flag; returns the stage count or -1. |
| `is_builtin` | Returns true if a command name is one of the six built-ins. |
| `builtin_cd`, `builtin_pwd`, `builtin_echo`, `builtin_env`, `builtin_setenv` | One function per built-in. |
| `run_builtin` | Dispatches to the correct built-in (also handles `exit`). |
| `apply_redirs` | Opens files and uses `dup2` to apply `<`, `>`, and `>>`. |
| `run_builtin_in_parent` | Runs a stand-alone built-in in the shell, saving and restoring stdin/stdout. |
| `run_pipeline` | Forks one child per stage, wires pipes, and execs. |
| `wait_foreground` | Waits for foreground PIDs with the 10-second timeout. |
| `reap_background` | Non-blocking cleanup of finished background children. |
| `main` | Input loop: prompt, read, tokenize, parse, execute. |

Defensive checks use `assert` where the code relies on an assumption, for example that `parse` is never called with zero tokens and that `run_builtin` receives at least one argument. System calls that can fail (`fork`, `pipe`, `open`, `dup2`, `chdir`, `getcwd`, `sigaction`) check their return values and report errors with `perror`.

## 5. Testing

I tested with scripted input and by comparing against the behavior described in the assignment:

- **Built-ins:** The example session from the assignment, including `echo $greeting $HOME`, `env greeting`, and both `setenv` forms.
- **External commands:** `ls -l`, `cp`, and a nonexistent command (`abc`), which prints the `execvp()` error followed by `An error occurred.`
- **Redirection and pipes:** `cat < file | grep x > out`, `>>` appending, two-stage and three-stage pipes, and a built-in inside a pipeline.
- **Background jobs:** `sleep 1 &` returns to the prompt immediately, and later commands run normally.
- **Timeout:** Compiled with `-DTIMEOUT_SECONDS=2`; `sleep 5` is terminated after 2 seconds, while shorter jobs return immediately.
- **Signals:** SIGINT sent to the shell at the prompt produced a new prompt and the shell kept running.
- **Malformed input:** A trailing `|`, a missing redirect target, and an unterminated quote each print an error and re-prompt.

## 6. Limitations and Possible Improvements

- Quoting is basic. There is no backslash escaping, and `$VAR` expands only when it is an entire unquoted word (so `echo foo$HOME` does not expand).
- There is no `~` expansion, wildcard globbing, `$?`, or `2>` stderr redirection.
- There is no job control (`jobs`, `fg`, `bg`), and background jobs do not report completion.
- A stand-alone built-in with `&` runs in the foreground, since it executes inside the shell.
- Lines are limited to 1024 characters, matching the starter code's buffer, and pipelines to 16 stages.
- The timeout uses `SIGKILL` for simplicity. A gentler version would send `SIGTERM` first and escalate after a short grace period.

## 7. Reflection

The most error-prone parts of this project were the ones that involve file descriptors and signals: closing every unused pipe end, flushing buffers before `fork`, and making Ctrl-C interrupt `fgets` correctly. These produce hangs or duplicated output instead of clean error messages, so careful reasoning about which process owns which descriptor mattered more than any individual line of code. Designing the parser and the `Stage` struct for the extra credit from the start meant pipes and redirection added little complexity at the end.
