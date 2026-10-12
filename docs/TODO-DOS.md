# TODO: gork on DOS

Status: working, experimental. `gork.exe` (built by `dosbuild.sh`, shipped
as `gork-<version>-dos.zip` since 0.5) passes `testing/dostest.py` on
FreeDOS 1.3 in QEMU: batch mode, the TUI, file tools, `run_command`, path
confinement and timeouts. What is still open is at the end of the list; the
findings are from before the port and explain why each change was needed.

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

- [x] Guard the two missing names for DJGPP (`#ifdef __DJGPP__`, or a
      `configure`-free `Makefile.dj`): no `SIGTTOU`, no `killpg`.
- [x] Add a `Makefile.dj` (or a documented one-line command) for the cross
      compiler and for DJGPP running on DOS itself; `configure` is not an
      option there.
- [x] `docs/BUILD.md` has a DOS section: packages, `Makefile.dj`, what to
      put beside `gork.exe`.

Network

- [x] Stand up a test machine: FreeDOS in QEMU with an emulated NE2000 and
      its packet driver, `WATTCP.CFG` with `my_ip = dhcp`, the fake API on
      the host at 10.0.2.2. DOSBox as shipped has no packet driver.
- [x] Check `relay.c` against Watt-32: `connect` with the `alarm` timeout,
      `select` and `close` on a socket. No change was needed for a relay
      given by address or by name. The timeout needed one: a `SIGALRM`
      inside Watt-32 aborts the program, so DOS polls a deadline instead.
- [x] Run a plain request (no tools) end to end.

Files and configuration

- [x] Config file and `_gork` directory: `gork.cnf` and `_gork`, under
      `HOME`, or in the directory of `gork.exe` when `HOME` is unset.
- [x] Skip the `api_key` permission check on DOS, where every file reads as
      `0644`.
- [x] `write_file`/`edit_file` write `NAME.gork-tmp` and rename it; pick a
      temporary name that fits 8.3 (`main.c`, around the `.gork-tmp` line).
- [x] `GORK.md` and `_gork/skills` in the working directory reach the
      model.
- [x] Path confinement: `c:\x`, `..\x`, `C:/W/../X`, `\x` and `a:\x` are
      all refused from `C:\W`; `in.txt` and `C:\W\IN.TXT` are read.

Tools

- [x] `run_command` without `fork`: run the command with `system()` or
      `spawn*()`, output redirected to a temporary file, then read the file.
      DOS runs one program at a time, so there is no timeout, no kill, and
      no Esc while a command runs. Say so in the tool description.
- [x] Command syntax is `COMMAND.COM`'s, not `/bin/sh`'s: change the tool
      description the model sees ("Command line for /bin/sh").
- [x] MCP: `mcp.json` is not read on DOS. The code is still compiled in.

TUI

- [x] `tui.c` captures stdout in `/tmp/gorkXXXXXX` and unlinks it while
      open. Use `TMPDIR`/`TEMP`, and delete the file at exit instead: DOS
      cannot remove an open file.
- [x] The screen is cleared when the TUI exits.
- [x] Esc aborts a waiting request and the trust prompt works. A running
      command cannot be interrupted on DOS; the hint line says so.
- [x] Key codes: Enter, Backspace, PgUp/PgDn and Esc work.

Testing and release

- [x] `testing/dostest.py` boots FreeDOS in QEMU and checks a batch round
      trip, path confinement, `run_command`, the timeout and the TUI.
- [x] A session against a real model (LM Studio on the host): batch tasks
      with `list_dir`, `read_file`, `edit_file`, `write_file` and
      `run_command`, and a TUI session with approvals, `/compact`,
      `/effort`, `/new`, scrolling and Esc.
- [x] Tool files are opened in text mode on DOS, so the model sees bare
      newlines and `edit_file` matches across lines of a CR LF file.
- [x] Long output is cut at the cap and says so; an `allow` directory on
      a second drive works (the list is `;`-separated on DOS).
- [x] Jenkins runs `dostest.py` after building the zip, in a container
      with qemu and mtools.
- [x] `mcp.c` is left out of `gork.exe`.
- [x] MS-DOS 6.22: `GORK_DOS_FLOPPY=<install disk 1> python3
      testing/dostest.py` passes all eight checks.
- [x] DR-DOS 7.03: passes the seven checks that need one disk
      (`GORK_DOS_ONE_DISK=1`); its install floppy does not see a second
      hard disk, so the `allow` check on D: could not run there.
- [x] An `allow` directory that does not exist is refused on DOS too
      (DJGPP's `realpath` does not check, so gork opens the directory).
- [ ] Find out why a `stat()` at startup breaks Esc. The first version of
      the allow check called `stat()` on each allow directory before the
      TUI starts, and with it Esc no longer aborted a waiting request (the
      request ran into its timeout instead); with `opendir()` it works.
      Narrowed down: Esc still works after a `list_dir` tool call, which
      also calls `stat()`, on C: and on D:. So it is something about the
      first `stat()` coming before curses is initialised.
- [ ] Real hardware. Everything above ran in QEMU with an emulated NE2000;
      a real 386 or 486 with a real packet driver has not been tried.
