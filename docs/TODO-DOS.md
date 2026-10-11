# TODO: gork on DOS

Status: investigated, not ported. gork cross-compiles for DOS today and the
binary starts, but it cannot yet complete a request or run a tool.

## Findings

Target: 32-bit protected mode with [DJGPP](https://www.delorie.com/djgpp/)
(386 or better, a DPMI host such as CWSDPMI). DJGPP's libc is close enough
to POSIX that almost all of gork compiles unchanged. A 16-bit real-mode
build (8086/286, Open Watcom) is not worth trying: requests and replies are
hundreds of kilobytes and do not fit in 640 K.

Tried with the DJGPP gcc 12.2 cross compiler on Linux, PDCurses 3.9
(`pdcur39a.zip`) and Watt-32 2.2.11 (`wat3211b.zip`), both from DJGPP's
`v2tk` directory:

```sh
i586-pc-msdosdjgpp-gcc -std=gnu89 -O -Ipdcurses/include -Iwatt/inc \
    -o gork.exe main.c relay.c tui.c mcp.c cJSON/cJSON.c \
    -Lpdcurses/lib -Lwatt/lib -lpdcurses -lwatt -lm
```

- Every file compiles. Only two things are missing: the `SIGTTOU` signal
  number and the `killpg` function, both in `main.c`'s `run_command`.
- With those two stubbed, `gork.exe` links (1.9 MB before stripping).
- In DOSBox it starts, reads the file named by `GORK_CONFIG`, and stops in
  Watt-32 with `NO PACKET DRIVER FOUND`, which is as far as DOSBox can go.

What a small probe program showed about DJGPP at run time, in DOSBox:

| Call | Result | Consequence |
|---|---|---|
| `fork()` | fails, always | `run_command` and MCP servers cannot start anything |
| `pipe()` | succeeds (emulated with a file) | not a real pipe: no concurrent reader and writer |
| `stat()` mode | always `0644` | a config file holding `api_key` is always refused |
| `realpath()` | works: `c:/g.cnf`, forward slashes, backslashes accepted | path confinement can stay as it is |
| `getenv("HOME")` | unset | no default config file, no `~/.gork` |
| `mkstemp("/tmp/gorkXXXXXX")` | fails without `C:\TMP` | the TUI cannot start |
| `fopen(".gork.conf")` | fails without long file names | dot files and `name.ext.gork-tmp` are not valid 8.3 names |
| `symlink()` | works (emulated) | nothing to do |
| `system()` | fails under DOSBox's built-in shell | test on FreeDOS or MS-DOS instead |

## To do

Build

- [ ] Guard the two missing names for DJGPP (`#ifdef __DJGPP__`, or a
      `configure`-free `Makefile.dj`): no `SIGTTOU`, no `killpg`.
- [ ] Add a `Makefile.dj` (or a documented one-line command) for the cross
      compiler and for DJGPP running on DOS itself; `configure` is not an
      option there.
- [ ] Say in `docs/BUILD.md` which PDCurses and Watt-32 packages to unpack
      and that `CWSDPMI.EXE` must sit next to `gork.exe`.

Network

- [ ] Stand up a test machine: FreeDOS in QEMU with an emulated NE2000 and
      its packet driver, `WATTCP.CFG` with `my_ip = dhcp`, the fake API on
      the host at 10.0.2.2. DOSBox as shipped has no packet driver.
- [ ] Check `relay.c` against Watt-32: `connect` with the `alarm` timeout,
      `select` and `close` on a socket (Watt-32 also has its own `select_s`
      and `close_s`; whether the plain calls work was not checked), and
      name lookup through `gethostbyname`.
- [ ] Run a plain request (no tools) end to end.

Files and configuration

- [ ] Config file: fall back to a name that is valid on 8.3 file systems
      when `HOME` is unset, for example `GORK.CNF` next to `gork.exe`, and
      likewise for `~/.gork` (`trusted`, `skills`, `mcp.json`).
- [ ] Skip the `api_key` permission check on DOS, where every file reads as
      `0644`.
- [ ] `write_file`/`edit_file` write `NAME.gork-tmp` and rename it; pick a
      temporary name that fits 8.3 (`main.c`, around the `.gork-tmp` line).
- [ ] `GORK.md` and `.gork/skills` in the working directory: decide the 8.3
      spellings (`GORK.MD` already fits; `.gork` does not).
- [ ] Path confinement: confirm with tests that `C:\DIR\..\X`, another
      drive letter (`D:`), and upper/lower case cannot step outside the
      working directory. `realpath` normalises the first; the others are
      untested.

Tools

- [ ] `run_command` without `fork`: run the command with `system()` or
      `spawn*()`, output redirected to a temporary file, then read the file.
      DOS runs one program at a time, so there is no timeout, no kill, and
      no Esc while a command runs. Say so in the tool description.
- [ ] Command syntax is `COMMAND.COM`'s, not `/bin/sh`'s: change the tool
      description the model sees ("Command line for /bin/sh").
- [ ] MCP: stdio servers need two programs running at once. Compile MCP out
      on DOS and report that `mcp.json` is ignored.

TUI

- [ ] `tui.c` captures stdout in `/tmp/gorkXXXXXX` and unlinks it while
      open. Use `TMPDIR`/`TEMP`, and delete the file at exit instead: DOS
      cannot remove an open file.
- [ ] Check the PDCurses calls gork uses (`newterm`, `keypad`, `nodelay`,
      `has_colors`, `reset_shell_mode`/`reset_prog_mode`) and the
      `tcgetattr`/`tcsetattr` use in `tui_suspend`. Esc as the interrupt key
      relies on termios `VINTR`, which DOS does not have.
- [ ] Key codes: Enter, Backspace, PgUp/PgDn and Esc as PDCurses reports
      them on a PC keyboard.

Testing and release

- [ ] `testing/looptest.py` cannot run on DOS. Drive batch and TUI round
      trips by hand, as on the oldest Unix systems, and write the steps
      down.
- [ ] Regression: none of this may change the Unix build; rerun the suite.
- [ ] Decide whether `gork.exe` becomes a release artifact (a `dos` stage in
      the Jenkinsfile using the cross compiler).
