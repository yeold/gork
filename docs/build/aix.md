# Building gork on AIX

Back to [all systems](../BUILD.md).

## AIX 4.3 on POWER

**AIX 4.3.3** (PowerPC). AIX ships no C compiler of its own; IBM's xlC
was sold apart, so use gcc (2.8.1 here, installed as `xgcc` on the disk
image used). Curses and the sockets are in the system libraries:

```sh
for f in main relay tui mcp cJSON/cJSON; do gcc -O -c $f.c; done
gcc -o gork main.o relay.o tui.o mcp.o cJSON.o -lcurses -lm
```

No source change is needed, and gcc gives no warnings. Batch mode, the
TUI (over telnet) and MCP on stdio and over HTTP work, tested in QEMU's
40p (an IBM RS/6000 7020) with a ready-installed disk image and the
patched QEMU 3.0 that came with it: QEMU 11.1's own 40p machine does not
get through the AIX boot. AIX's `grep` refuses lines longer than 2048
bytes, and a running program cannot be overwritten (`ld` says the file
is in use).

## AIX PS/2

**AIX PS/2 1.3** (1992, i386, MetaWare High C R2.2g). The compiler is a
package of its own, on the "Metaware C Compiler" diskette: install it with
`installp`; without it `cc` only says that the compiler is not available.
There is no `configure` route; compile by hand, with `aixps2.c` for what
the system lacks:

```sh
cc -c main.c relay.c tui.c mcp.c aixps2.c cJSON/cJSON.c
cc -o gork main.o relay.o tui.o mcp.o aixps2.o cJSON.o -lcurses -lm
```

`cc` rejects `-o` together with `-c`, so the objects land in the current
directory. Batch mode and the TUI work, tested in 86Box (an emulated PS/2
Model 80) against a fake API over loopback only: 86Box has no network
card that AIX PS/2 has a driver for (see [TODO-REAL.md](../TODO-REAL.md)).
