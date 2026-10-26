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
or the program's folder. Command-period interrupts.

A file `gork.in` beside the program is read in place of the keyboard,
and what the window shows goes to `gork.out` too. A line `shutdown` at
its end turns the Mac off: the tests run it so.

## Paths

gork writes paths as `/Volume/Folder/file`; the Mac's
`Volume:Folder:file` is the same place. A `/` in a Mac name is a `:` to
gork. `/` alone lists the volumes. Full paths stop at the File Manager's
255 characters. Files gork makes are SimpleText's (`TEXT`, `ttxt`).

## The network

MacTCP, set up in its control panel (or the TCP/IP control panel of Open
Transport). Host names go through the resolver of that control panel;
give the relay's address if there is none. The relay does TLS, as
everywhere (README).

## Build

```sh
RETRO68=/path/to/Retro68-build/toolchain sh macbuild.sh [OUT]
```

Needs [Retro68](https://github.com/autc04/Retro68) built for 68k
(`build-toolchain.bash --no-ppc --no-carbon`), with its Multiversal
Interfaces; python3 and zip.

## How it works

As for barebox and GRUB: gork's half is `main.c`, `relay.c`, cJSON,
`fwlibc.c` and `barebox/libc.c`, built against `fwinclude/` (and
`mac/include/` for `stdint.h`) with every C library name made `gork_NAME`
(`barebox/rename.py`) and joined into one object (`barebox/gork.lds`).
`mac/glue.c`, built with Retro68's headers and C library, gives the calls
of `barebox/bb.h` on the File Manager, the clock and Retro68's console
window, and those of `ip.h` on MacTCP in place of gork's own TCP/IP.
Before each command line it puts gork's variables back as they were at
the start, and comes back from `exit()` by `longjmp`. It moves the stack
limit down 512K first, as a 68k Mac gives a program about 24K of stack.

Retro68's console window is C++, and brings most of libstdc++ with it:
the program is about 1 MB, of which gork is about 64K.
