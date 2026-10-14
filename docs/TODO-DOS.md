# TODO: gork on DOS

Both DOS builds work and pass `testing/dostest.py`: the 32-bit one
(`dosbuild.sh`, DJGPP, a 386 or better) and the 16-bit one
(`dos16build.sh`, Open Watcom, real mode). [BUILD.md](BUILD.md) says how
they are built and what each needs. What is left:

- [ ] Find out why a `stat()` at startup breaks Esc in the 32-bit build.
      The first version of the allow check called `stat()` on each allow
      directory before the TUI starts, and with it Esc no longer aborted a
      waiting request (the request ran into its timeout instead); with
      `opendir()` it works. Narrowed down: Esc still works after a
      `list_dir` tool call, which also calls `stat()`, on C: and on D:. So
      it is something about the first `stat()` coming before curses is
      initialised. Still so with `_djstat_flags` set (2026-10-14): one
      `stat()` or `lstat()` at startup, on a file or a directory, breaks
      it, while linking it in without calling it does not, and neither
      does any of its parts called alone (`_fixpath`, `_truename`,
      `findfirst`, `_get_cached_blksize`, `mktime`, the FSEXT hook,
      symlink resolution). What breaks: Watt-32's `select()` overruns its
      200 ms until the request's deadline, so Esc is never polled; in
      batch mode a tool call can hang. `hires_timer(0)` is no way out:
      then `connect` times out.

## What DOS lacks, and what gork does about it

| | On DOS | So |
|---|---|---|
| `fork()` | none | `run_command` runs the command with its output in a temporary file, with no timeout and no interrupt; no MCP servers |
| File modes | every file reads as `0644` | the `api_key` permission check is skipped |
| `HOME` | unset | the config is `gork.cnf` beside `gork.exe`, the directory `_gork` |
| File names | 8.3, no leading dot | `gork.cnf`, `_gork`, and `NAME.g$$` for the temporary file of a write |
| Signals | a `SIGALRM` inside Watt-32 aborts the program | the request timeout is a deadline that the read polls |
| An open file | cannot be removed | the TUI deletes its capture file at exit |
| Text files | CR LF | tool files are opened in text mode: the model sees bare newlines |
| The shell | `COMMAND.COM` | the tool description says so |
