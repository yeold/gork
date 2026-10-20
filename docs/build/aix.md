# Building gork on AIX PS/2

Back to [all systems](../BUILD.md).

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
AIX on POWER is untested.
