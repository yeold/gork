# TODO: gork on DOS

Both DOS builds work and pass `testing/dostest.py`: the 32-bit one
(`dosbuild.sh`, DJGPP, a 386 or better) and the 16-bit one
(`dos16build.sh`, Open Watcom, real mode). [BUILD.md](BUILD.md) says how
they are built and what each needs. Nothing is open here; what only real
hardware can show is in [TODO-REAL.md](TODO-REAL.md).

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
