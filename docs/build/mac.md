# gork on the classic Mac OS

Work in progress: this builds, but has not yet run on a Mac or in an
emulator. What follows is how it is meant to work.

gork for System 7 on a 68k Mac (a 68020 or better) is a program with a
console window, in batch mode, on MacTCP (or Open Transport's MacTCP).
There is no shell on a Mac, so no `run_command`, and no TUI and no MCP,
as on DOS.

## Run

`gork-<version>-mac68k.zip` has the program as MacBinary (`gork.bin`)
and on an HFS disk image (`gork.dsk`). Put `gork.cnf` beside the program:

```
relay = 192.168.1.10:8443
api_key = sk-ant-...
```

Start it, and type gork command lines at its prompt:

```
gork> "write hello.txt with a greeting in it"
gork> set GORK_DIR=/Macintosh HD/Work
gork> --version
gork> quit
```

`set NAME=value` and `unset NAME` set what gork would take from the
environment (`GORK_DIR`, `GORK_CONFIG`, ...). gork works in `GORK_DIR`,
or the program's folder. Command-period interrupts; the close box and
Command-Q quit. The window is in Mac Roman, and gork in UTF-8: what gork
prints is shown in Mac Roman (a `?` where it has no such character), and
what is typed reaches gork as UTF-8.

A file `gork.in` beside the program is read in place of the keyboard,
and what the window shows goes to `gork.out` too. A line `shutdown` at
its end turns the Mac off: the tests run it so.

## Paths

gork writes paths as `/Volume/Folder/file`; the Mac's
`Volume:Folder:file` is the same place. A `/` in a Mac name is a `:` to
gork. `/` alone lists the volumes. Full paths stop at the File Manager's
255 characters. Files gork makes are SimpleText's (`TEXT`, `ttxt`).

A text file (`TEXT`) is in Mac Roman on the Mac, its lines ending in CR;
gork's text is UTF-8, its lines ending in LF. gork reads and writes a
text file in the one and sees it in the other, so SimpleText shows what
gork wrote and gork reads what SimpleText wrote. A file read and written
again is as it was, but what Mac Roman has no character for (most of
Unicode: emoji, Greek, Cyrillic, CJK) is written as `?`. Sizes are the
Mac's, in bytes of Mac Roman. Other files are read and written as they
are.

## The network

MacTCP, set up in its control panel (or the TCP/IP control panel of Open
Transport). Host names go through the resolver of that control panel;
give the relay's address if there is none. The relay does TLS, as
everywhere (README).

## Build

```sh
sh macbuild.sh [OUT] [WORK]
```

The first run builds [Retro68](https://github.com/autc04/Retro68), the
cross toolchain, for 68k into `WORK` from a pinned commit, with its
Multiversal Interfaces: about 40 minutes, and 9 GB while it builds; the
toolchain kept is about 450 MB. Later runs use it. `RETRO68=dir` uses a
Retro68 toolchain built already instead. Needs git, gcc and g++, make,
cmake, bison, flex, texinfo, ruby, the GMP, MPFR, MPC, Boost and zlib
headers (for Retro68), python3 and zip.

## How it works

As for barebox and GRUB: gork's half is `main.c`, `relay.c`, cJSON,
`fwlibc.c` and `barebox/libc.c`, built against `fwinclude/` (and
`mac/include/` for `stdint.h`) with every C library name made `gork_NAME`
(`barebox/rename.py`) and joined into one object (`barebox/gork.lds`).
`mac/glue.c`, built with Retro68's headers and C library, gives the calls
of `barebox/bb.h` on the File Manager, the clock and its console window
(`mac/console.c`, a QuickDraw window of Monaco 9), and those of `ip.h`
on MacTCP in place of gork's own TCP/IP.
Before each command line it puts gork's variables back as they were at
the start, and comes back from `exit()` by `longjmp`. It moves the stack
limit down 512K first, as a 68k Mac gives a program about 24K of stack.
The program is about 190K: Retro68's own console window, in C++, would
bring most of libstdc++ with it, a megabyte.
