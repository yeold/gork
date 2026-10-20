# Building gork on HP-UX

Back to [all systems](../BUILD.md).

**HP-UX 11i v1 (11.11)** on PA-RISC 1.1. The bundled `cc` is K&R only,
and the gcc depots that can still be found are built for PA-RISC 2.0. On
a PA-RISC 2.0 machine with gcc installed, the
[Everywhere](../BUILD.md#everywhere) route should work, but it has not been tried.

What was tried is a cross compiler that stops at assembly, with the
system's own `as` and `ld` doing the rest. gcc 12 is the last release
that targets 32-bit HP-UX 11. Build only the compiler, on Linux:

```sh
CXX="g++ -std=gnu++14" ../gcc-12.4.0/configure \
    --target=hppa1.1-hp-hpux11.11 --enable-obsolete --enable-languages=c \
    --without-headers --without-gnu-as --without-gnu-ld \
    --disable-shared --disable-threads --disable-nls --prefix=$HOME/cross
make all-gcc && make install-gcc
```

Copy `/usr/include` from the HP-UX system to `sysroot/usr/include`, then
compile each source to assembly. `-mno-gas` makes the output something
HP's assembler accepts:

```sh
G=$HOME/cross/bin/hppa1.1-hp-hpux11.11-gcc
I=$HOME/cross/lib/gcc/hppa1.1-hp-hpux11.11/12.4.0/include
for f in main relay tui mcp cJSON/cJSON; do
    $G -O2 -mno-gas -std=gnu89 -nostdinc -isystem $I \
       -isystem sysroot/usr/include -I. -S $f.c -o $(basename $f).s
done
```

On HP-UX, assemble and link:

```sh
for f in main relay tui mcp cJSON; do /usr/ccs/bin/as -o $f.o $f.s; done
cc -o gork main.o relay.o tui.o mcp.o cJSON.o -lcurses -lm
```

The code calls nothing from libgcc, only HP's millicode, which `cc`
links in.

**HP-UX 10.20** works the same way, with a compiler built for
`--target=hppa1.1-hp-hpux10.20` and 10.20's own `/usr/include` as the
sysroot. Two things differ. Its plain curses has no colour, so compile
against the colour one (add `-isystem sysroot/usr/include/curses_colr`
before the other sysroot path) and link `-lcur_colr`. And gcc's 10.20
target calls `__main` at the start of `main`; gork has no constructors,
so an empty one does, compiled by HP's own `cc`:

```sh
echo '__main(){}' > m.c && cc -c m.c
cc -o gork main.o relay.o tui.o mcp.o cJSON.o m.o -lcur_colr -lm
```

The install media from 1996 and 1997 find no disk in QEMU's B160L; a
1998 one (part B3782-10456) does.

On both, batch mode, the TUI (`TERM=vt100` on the serial console) and
MCP work (on 10.20 also on stdio, and Esc stops a command). They were
tested in QEMU's B160L, with the fake API across QEMU's user network.
