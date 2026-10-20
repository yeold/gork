# Building gork on System V Release 4.0

Back to [all systems](../BUILD.md).

**Dell Unix SVR4 Issue 2.2** (1992) on the 386. Its base system has
AT&T's C compiler, curses and the socket libraries; `-Xa` asks for ANSI C,
and `svr40.c` supplies `mkstemp`:

```sh
for f in main relay tui mcp svr40 cJSON/cJSON; do cc -Xa -O -c $f.c; done
cc -o gork main.o relay.o tui.o mcp.o svr40.o cJSON.o \
    -lcurses -lsocket -lnsl -lm
```

The compiler predefines only `i386` and `unix`, so `port.h` takes an i386
Unix compiler that is not gcc and whose headers have no `ssize_t` for
SVR4.0. Its `tar` reads only the old v7 format (`tar --format=v7` on the
machine that packs the source). Batch mode, the TUI (over telnet) and MCP
on stdio and over HTTP work, tested in 86Box with a WD8013 card: Dell's
`wdn` driver, set to its port, IRQ and memory in
`/etc/conf/sdevice.d/wdn`, with the Intel 82586 driver turned off, whose
memory range overlaps. The floppy images on WinWorld are sound; its tape
images have had every line feed turned into CR LF and do not install.
