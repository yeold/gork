# Building gork

Build instructions per operating system. Everything here was run on the
release named; the table of what was tested is in the
[README](../README.md#tested-systems).

- [Everywhere](#everywhere)
- [Linux](#linux)
- [FreeBSD](#freebsd)
- [NetBSD](#netbsd)
- [OpenBSD](#openbsd)
- [Solaris](#solaris)
- [illumos](#illumos)
- [Haiku](#haiku)
- [DOS](#dos)
- [No curses headers](#no-curses-headers)

## Everywhere

gork needs a C89 compiler, make, and a curses library with its headers.
cJSON is vendored; the only link dependencies are curses and `-lm`.

From a release tarball, which ships a ready-made `configure`:

```sh
./configure
make
make install        # /usr/local/bin; change with ./configure --prefix=...
```

From a git checkout, generate `configure` first. This needs autoconf and
automake:

```sh
autoreconf -i
./configure
make
```

To build on a machine without autotools, run `make dist` on one that has
them and copy over the resulting `gork-<version>.tar.gz`. On old systems
always use the tarball: autoconf 2.13 and automake 1.4 cannot regenerate
`configure`.

Where `configure` cannot run at all, one compiler command builds gork. The
per-system sections give the exact flags; the general shape is:

```sh
gcc -O -o gork main.c relay.c tui.c mcp.c cJSON/cJSON.c -lncurses -lm
```

Useful `configure` arguments:

| Argument | When |
|---|---|
| `LIBS=-lncursesw` | UTF-8 terminal, so non-ASCII text renders |
| `CPPFLAGS=-I/usr/include/ncurses` | `/usr/include/curses.h` is not ncurses but ncurses is installed |
| `CPPFLAGS=-I/path/include LDFLAGS=-L/path/lib` | curses lives outside the system directories |

gcc older than 2.95 prints `unrecognized option -std=gnu89` for every file.
That is harmless.

## Linux

**Current distributions.** Install a compiler, make and the ncurses headers
(`libncurses-dev` on Debian and Ubuntu, `ncurses-devel` on Fedora and
openSUSE), plus autoconf and automake for a git checkout. Then build as
under [Everywhere](#everywhere).

**SuSE 8.1** (gcc 3.2, glibc 2.2). Builds from a git checkout; its autoconf
and automake are new enough. Without the `ncurses-devel` package, see
[No curses headers](#no-curses-headers).

**Debian 1.3, 2.1, 2.2** (gcc 2.7.2 to 2.95, libc 5.4 to glibc 2.1). From
the tarball, with no flags:

```sh
./configure
make
```

**Slackware 3.0** (gcc 2.7.0, libc 5.0.9, ncurses 1.9.4). The `curses.h` in
`/usr/include` is BSD curses, so point at the ncurses one:

```sh
./configure CPPFLAGS=-I/usr/include/ncurses
make
```

**Slackware 2.0.0** (Linux 1.0.9, gcc 2.5.8, libc 4.5.26, ncurses 1.8.5).
The oldest system gork runs on. `configure` is no use: that kernel reads
the clock as 1926, so make sees files from the future and reruns it
forever. Compile by hand:

```sh
gcc -O -I/usr/include/ncurses -o gork main.c relay.c tui.c mcp.c \
    cJSON/cJSON.c -lncurses -lm
```

**Slackware 1.01** (libc 4.4.1) does not work: its libc has no `realpath`
and its ncurses no `has_colors`.

## FreeBSD

**FreeBSD 15.1** (clang 19). The base system has everything needed for a
tarball build:

```sh
./configure
make
```

A git checkout additionally needs autoconf and automake; that route was
not tried here.

**FreeBSD 2.0 and 2.0.5** (gcc 2.6). `/bin/sh` cannot run `configure`.
`curses.h` is BSD curses and ncurses is in `ncurses.h`, so give the
compiler a directory where `curses.h` is the ncurses header:

```sh
mkdir inc && ln -s /usr/include/ncurses.h inc/curses.h
gcc -O -Iinc -o gork main.c relay.c tui.c mcp.c cJSON/cJSON.c \
    -lncurses -lmytinfo -lm
```

**FreeBSD 1.x** does not work: its libc has no `realpath`.

## NetBSD

**NetBSD 10.1.** A fresh clone builds with the system curses and make:

```sh
autoreconf -i
./configure
make
```

**NetBSD 1.0** (gcc 2.4.5). The system curses has no keypad or colour
support, so build ncurses first. ncurses 4.2 works:

```sh
# in the ncurses-4.2 source tree
./configure --prefix=/usr/local --without-cxx --without-ada
make && make install
```

`/bin/sh` cannot run gork's `configure`, so compile by hand:

```sh
gcc -O -I/usr/local/include -o gork main.c relay.c tui.c mcp.c \
    cJSON/cJSON.c -L/usr/local/lib -lncurses -lm
```

## OpenBSD

**OpenBSD 7.9** (clang 19). From the tarball, with the base system only:

```sh
./configure
make
```

The linker warns about `sprintf`, `strcpy` and `strcat`. That is OpenBSD's
standard advice, not an error.

**OpenBSD 2.0** (gcc 2.7.2.1). `/bin/sh` cannot run `configure`. libcurses
is already ncurses:

```sh
gcc -O -o gork main.c relay.c tui.c mcp.c cJSON/cJSON.c \
    -lcurses -ltermlib -lm
```

## Solaris

`configure` picks the System V curses and adds `-lsocket -lnsl` by itself.
Solaris has no compiler in its default `PATH`, so set that first. Build from
the tarball.

**Solaris 10** (bundled gcc 3.4.3). On top of a Core install it needs
these packages from the install DVD: `SUNWgcc`, `SUNWgccruntime`,
`SUNWbinutils`, `SUNWhea`, `SUNWarc`, `SUNWcpp`, `SUNWbtool`, `SUNWsprot`,
`SUNWtoo`, `SUNWlibm`, `SUNWlibms` and `SUNWzlib`. Then:

```sh
PATH=/usr/sfw/bin:/usr/ccs/bin:$PATH; export PATH
./configure
make
```

**Solaris 2.6 and 2.5.1.** Install the Developer System Support group and a
gcc package (gcc 2.8.1 on 2.6 and gcc 2.7.2 on 2.5.1 were used, both
installing into `/usr/local`):

```sh
PATH=/usr/local/bin:/usr/ccs/bin:$PATH; export PATH
./configure
make
```

Solaris 2.4 is untested.

## illumos

**OmniOS.** Install the compiler and headers, then build from the tarball:

```sh
pkg install developer/gcc14 system/header developer/object-file \
    developer/linker
./configure
make
```

That uses the System V curses. To use ncurses instead:

```sh
./configure CPPFLAGS=-I/usr/include/ncurses
```

**OpenSolaris build 134.** Same, with the packages of its day:

```sh
pkg install developer/gcc-3 system/header developer/object-file \
    developer/linker
./configure
make
```

## Haiku

Install the `ncurses6_devel` package, then build as under
[Everywhere](#everywhere). `configure` finds sockets and the resolver in
`libnetwork`. Confirmed on R1/beta6.

## DOS

Experimental; [TODO-DOS.md](TODO-DOS.md) has what works and what is left.
32-bit protected mode only: a 386 or better, built with
[DJGPP](https://www.delorie.com/djgpp/), with PDCurses for the screen and
Watt-32 for TCP/IP. Tested on FreeDOS 1.3.

On Linux, `dosbuild.sh` downloads a DJGPP cross compiler and both libraries
and leaves `gork-<version>-dos.zip` in `out/`:

```sh
sh dosbuild.sh
```

With a DJGPP you already have, on DOS or as a cross compiler, unpack
`pdcur39a.zip` and `wat3211b.zip` from DJGPP's `v2tk` directory and run:

```sh
make -f Makefile.dj CC=i586-pc-msdosdjgpp-gcc PDCURSES=/path/to/pdcurses \
    WATT=/path/to/watt/net/watt
```

`python3 testing/dostest.py` then checks that zip on FreeDOS in QEMU.

To run it, the machine needs:

- `CWSDPMI.EXE` beside `gork.exe` (it is in the zip);
- a packet driver for the network card, loaded first;
- a `WATTCP.CFG` with the address settings, found through the `WATTCP.CFG`
  environment variable or in the current directory;
- `gork.cnf` beside `gork.exe`, or wherever `HOME` or `GORK_CONFIG` point.

DOS differences: file names are 8.3, so the config is `gork.cnf` and the
directory is `_gork`; `run_command` takes `COMMAND.COM` syntax and cannot be
timed out or interrupted; MCP servers are not available.

## No curses headers

No ncurses headers and no way to install them (SuSE 8.1 without
`ncurses-devel`, for example)? Build a static ncurses into your home
directory. gork then needs only libc at runtime:

```sh
# in an unpacked ncurses-5.9 source tree
./configure --prefix=$HOME/local --without-shared --without-cxx \
    --without-ada --without-progs --without-tests --without-manpages \
    --enable-overwrite --with-terminfo-dirs=/usr/share/terminfo \
    --with-default-terminfo-dir=/usr/share/terminfo
make libs && make install.libs install.includes
```

Then build gork as usual: `configure` picks up `~/local` by itself when the
system has no `curses.h`. Anywhere else, point it there by hand:

```sh
./configure CPPFLAGS=-I/path/include LDFLAGS=-L/path/lib
```
