# Building gork

Build instructions per operating system. Everything here was run on the
release named; the table of what was tested is in the
[README](../README.md#tested-systems).

- [Everywhere](#everywhere)
- [Linux](#linux)
- [FreeBSD](#freebsd)
- [NetBSD](#netbsd)
- [OpenBSD](#openbsd)
- [DragonFly BSD](#dragonfly-bsd)
- [Solaris](#solaris)
- [illumos](#illumos)
- [Minix](#minix)
- [GNU/Hurd](#gnuhurd)
- [Haiku](#haiku)
- [macOS](#macos)
- [Cygwin](#cygwin)
- [DOS](#dos)
- [Windows](#windows)
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

Cross-compiling works the usual autoconf way, given a cross compiler and
the target's curses library and headers:

```sh
./configure --host=s390x-linux-gnu
make
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
openSUSE, `build-base ncurses-dev` on Alpine, which uses musl), plus autoconf and automake for a git checkout. Then build as
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
not tried here. MidnightBSD 4.0.8 builds the same way.

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

**NetBSD 10.1 and 11.0.** A fresh clone, or the tarball, builds with the
system curses and make (11.0 also passes the whole test suite):

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

## DragonFly BSD

**DragonFly 6.4.** The compiler is in the base system, but its ncurses is
private to the base tools. Use the `ncurses` package, which needs two
include directories:

```sh
pkg install ncurses
./configure CPPFLAGS="-I/usr/local/include -I/usr/local/include/ncurses" \
    LDFLAGS=-L/usr/local/lib
make
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

## Minix

**Minix 3.3.0.** The base system has make and curses but no compiler.
Install `clang` and `binutils` from the 3.3.0 package set (`pkgin`, or
`pkg_add -f` on the downloaded files; do it under `/usr`, the root file
system is too small), then build from the tarball:

```sh
./configure CC=clang
make
```

Batch mode and the TUI work. On the Minix console itself the TUI's status
line scrolls off the top after the first reply; the cause is not found yet.

## GNU/Hurd

**Debian GNU/Hurd.** `apt-get install gcc make libncurses-dev`, then build
as under [Everywhere](#everywhere). The Hurd has no `PATH_MAX`; gork
assumes 4096 there.

## Haiku

Install the `ncurses6_devel` package, then build as under
[Everywhere](#everywhere). `configure` finds sockets and the resolver in
`libnetwork`. Confirmed on R1/beta6; older releases may work but are
untested.

## macOS

Releases carry `gork-<version>-macos.pkg`, which installs
`/usr/local/bin/gork`, and `gork-<version>-macos.tar.gz` with the same
binary: for Apple silicon and Intel, macOS 11 or later, built by GitHub
Actions. Neither is signed, so macOS asks before it opens one that a
browser downloaded: right-click the package and choose Open, or run
`xattr -d com.apple.quarantine` on the file.

**macOS 26** (Apple clang 21, arm64). The compiler comes with the Xcode
command line tools and curses with the system. A git checkout also needs
`brew install autoconf automake`. Then build as under
[Everywhere](#everywhere).

## Cygwin

**Cygwin 3.6** (gcc 14, ncurses 6.5). Install `gcc-core`, `make` and
`libncurses-devel` with Cygwin's setup program, plus `autoconf` and
`automake` for a git checkout, and `python3` for the test suite. Then
build as under [Everywhere](#everywhere), in a Cygwin shell. Check the
source out with LF line ends (`git config core.autocrlf input`): with CRLF
`autoreconf` fails.

**The Windows package.** `gork-<version>-win32.zip` on the releases is
this build, packed by `cygpack.sh` to run without a Cygwin install:
`sh cygpack.sh` after `make`, with the `dash` and `zip` packages
installed. It holds `gork.exe`, the Cygwin DLLs it loads, `dash` as
`bin\sh.exe` for `run_command` and MCP servers, and four terminal
descriptions; `gork.cmd` starts it with `HOME` set to the user's profile,
so the config is `%USERPROFILE%\.gork.conf`. Cygwin takes the directory
above `bin` as `/`, which is how `/bin/sh`, the terminal descriptions and
`/tmp` are found inside the package: it has to stay in one piece, in a
place the user can write to. The shell finds Windows programs on the
`PATH`; Unix tools are there only if the user has them. 64-bit Windows 10
and later, as Cygwin itself. GitHub Actions builds the package, runs
`testing/cygpacktest.py` on it with nothing of Cygwin on the `PATH`, and
adds it to the release on a tag. The TUI of the package has not been
looked at.

The older, native build is `gork-<version>-win32-legacy.zip`
([Windows](#windows)): it runs on everything back to Windows 95, and has
no MCP.

## DOS

Experimental; [TODO-DOS.md](TODO-DOS.md) has what is left.
32-bit protected mode only: a 386 or better, built with
[DJGPP](https://www.delorie.com/djgpp/), with PDCurses for the screen and
Watt-32 for TCP/IP. Tested on FreeDOS 1.3, MS-DOS 3.30, 4.01 and 6.22,
and DR-DOS 7.03.

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

`python3 testing/dostest.py` then checks that zip on FreeDOS in QEMU, or
on another DOS whose boot floppy image `GORK_DOS_FLOPPY` names
(`GORK_DOS_SMALL_DISKS=1` as well for MS-DOS 3.30 and 4.01, which read
only disks laid out the old way).

**Under Windows 3.0 and 3.1** the DOS `gork.exe` runs in a DOS box, in 386
enhanced mode: Windows provides the protected mode there, so `CWSDPMI.EXE`
is not used. It passes the same nine checks (`GORK_DOS_WINDOWS` names an
installed `WINDOWS` directory for `dostest.py`). Load the packet driver
before Windows, and `WINPKT` after it (`WINPKT 0x60`, from the same Crynwr
collection): without `WINPKT`, gork dies at its first disk access during a
request and the network is dead afterwards. The Windows `gork.exe` is of
no use here: Windows 3.x cannot run 32-bit console programs, with or
without Win32s.

**Under Windows 1 and 2** it passes the nine checks as well: Windows 1.04
on MS-DOS 3.30, Windows 2.03 and 2.11 (the 286 edition) on MS-DOS 4.01.
Each wants a DOS of its time: Windows 2.11 runs no DOS program at all on
MS-DOS 6, and Windows 1.04 hangs on MS-DOS 4.01 as soon as it starts a
program. Three things have to be arranged:

- `CWSDPMI -p` before `WIN`, to have it resident. Loaded inside Windows,
  it hangs.
- A PIF for the shell gork is started from, asking for the memory: without
  one a DOS program gets a window and 52K. Windows 2 always runs
  `COMMAND.COM` in a window, whatever `COMMAND.PIF` says (Windows 1 was
  not asked), and looks each
  program started there up again. So copy `COMMAND.COM` under another name
  and give that a PIF (`dostest.py` makes one from `WORD.PIF`: full
  screen, 400K required, 640K desired); gork then runs inside its box.
- The packet driver loaded before Windows, as usual. No `WINPKT`: there is
  only one machine.

Without TCP/IP, on an IPX network, gork can reach a
[gork-relay](https://github.com/yeold/gork-relay) over SPX: load an IPX
driver that has SPX (`IPXODI`, `IPX.COM`, or `PDIPX` after a packet
driver) and set `relay = spx:NET.NODE:SOCKET`. Both DOS builds have it,
and `dostest.py` checks it when `GORK_RELAY` names a relay binary.

**The 16-bit build** (`gork-<version>-dos16.zip`) is the same source
compiled with [Open Watcom](https://github.com/open-watcom/open-watcom-v2)
for real mode and the large memory model: it needs no 386, no DPMI and no
`CWSDPMI.EXE`, only DOS, a packet driver and about 400K of free memory.
`sh dos16build.sh` downloads the compiler and builds Watt-32 and PDCurses
for it, on x86-64 Linux. What 640K and a 16-bit `int` change
(`GORK_DOS16` in the source):

- No block of memory passes 64K and cJSON prints at most 32K, so the
  conversation is kept small: `context_window` defaults to 8000 tokens
  and `auto_compact` to 50, a reply to 3000 tokens, `read_file` and
  `run_command` return at most 8K, and `edit_file` takes files up to 30K.
- Watt-32 is built without its optional parts: no BSD sockets (gork uses
  its own calls there), no DHCP, so the address goes in `WATTCP.CFG`.
- The linker must not pack code segments into groups
  (`option packcode=1`), or Watt-32's interrupt code crashes at
  `sock_init()`, on some layouts and not on others.

It passes the nine `dostest.py` checks, TUI included, on FreeDOS, MS-DOS
3.30 and 6.22 and in a DOS box under Windows 1.04, 2.11/386 and 3.1. All
of that on QEMU's 486: it is compiled for the 8086, but no 8086 or 286
has run it.

To run it, the machine needs:

- `CWSDPMI.EXE` beside `gork.exe` (it is in the zip);
- a packet driver for the network card, loaded first;
- a `WATTCP.CFG` with the address settings, found through the `WATTCP.CFG`
  environment variable or in the current directory;
- `gork.cnf` beside `gork.exe`, or wherever `HOME` or `GORK_CONFIG` point.

DOS differences: file names are 8.3, so the config is `gork.cnf` and the
directory is `_gork`; `allow` directories are separated by `;`, as `:`
belongs to the drive letter; text files keep their CR LF line ends while the
model sees plain newlines; `run_command` takes `COMMAND.COM` syntax and
cannot be timed out or interrupted; MCP servers are not available.

## Windows

Experimental. Besides the [Cygwin](#cygwin) build there is a native one:
a 32-bit `gork.exe` built with MinGW (mingw-w64), with PDCurses for the
screen and Winsock 1.1 for TCP/IP. It is one program for both lines of
Windows, 9x and NT: it imports only functions that Windows 95 already has,
from `kernel32`, `user32`, `advapi32`, `wsock32` and `msvcrt.dll`.

Tested on Windows NT 3.51 Workstation, the first Windows 95 (4.00.950),
Windows 95 OSR2 (4.00.1111), Windows 98 Second Edition, Windows Me
(4.90.3000), Windows NT 4.0
Workstation (as shipped, no service pack), Windows 2000 Professional
(5.00.2195) and Windows XP Professional SP3, in QEMU, the older ones on an
emulated Pentium or 486: batch mode, every file tool, `run_command` through
`COMMAND.COM` or `cmd.exe`, and the TUI, all by hand against a fake API.
`testing/wintest.py` runs the batch-mode checks under Wine 10 (Jenkins)
and on the Windows of GitHub's runners, and the release zip of 0.9~beta is
reported working on Windows 11, TUI included. Windows Vista to 10 were
not tried; XP and 11, on either side of them, work.

**Windows NT 3.1 and 3.5** take `gorknt3.exe`, which is in the same zip.
They refuse a program marked for Windows 4.0, as `gork.exe` is, and no
`msvcrt.dll` helps them: the one of a later Windows does not load there.
`gorknt3.exe` is the same source marked for Windows 3.10 and linked
against `crtdll.dll`, which every Windows NT has (`make -f Makefile.w32
gorknt3.exe`; `nt3crt.c` has the four functions MinGW's library misses
there). It passes `wintest.py` under Wine and on a current Windows, and on
NT 3.1 in QEMU ran the batch round trip and the TUI. NT 3.1 installs no
TCP/IP by itself: add it in the Network control panel, with a fixed
address, as it has no DHCP. On a FAT disk it knows only 8.3 names. NT 3.5
was not tried.

**Windows 95 and NT 3.51 have no `msvcrt.dll`**, and gork.exe will not
start without it ("A required .DLL file, MSVCRT.DLL, was not found"). Put a copy beside
`gork.exe` or in the system directory; the ones from Windows 98 and NT 4
work, and
Internet Explorer 4 and many applications install one. gork does not
include the file, in the repository or in the release zip: it is
Microsoft's, and not ours to pass on. Neither
installs TCP/IP unless asked: add the protocol in the Network control
panel. NT 3.51 sets no `USERPROFILE`, so there `gork.cnf` goes beside
`gork.exe`, as on Windows 9x.

It is compiled for the 486, but two instructions of MinGW's start-up code,
which Debian ships built for the Pentium Pro, remain; a Pentium did not
run into them, a real 486 was not tried.

On Linux, `winbuild.sh` downloads the PDCurses source and leaves
`gork-<version>-win32-legacy.zip` in `out/` ("legacy" since the
[Cygwin package](#cygwin) took the plain name; this build has no MCP: it
has no `fork` to start the servers with). It needs the 32-bit mingw-w64 gcc
(`gcc-mingw-w64-i686` on Debian):

```sh
sh winbuild.sh
```

With a MinGW you already have, on Windows or as a cross compiler, build
`wincon/pdcurses.a` in the PDCurses 3.9 source and run:

```sh
make -f Makefile.w32 CC=i686-w64-mingw32-gcc PDCURSES=/path/to/PDCurses-3.9
```

`python3 testing/wintest.py wine gork.exe` then checks it under Wine, and
`python testing\wintest.py gork.exe` on Windows.

The config is `gork.cnf` and the directory `_gork`, as on DOS (the shell
and Explorer of Windows 9x refuse names that start with a dot), in `%HOME%`
if that is set, else in `%USERPROFILE%`, else (Windows 9x) beside
`gork.exe`.

Windows differences, mostly those of DOS: `allow` directories are separated
by `;`; paths may be written with either slash and in either case; text
files are written with CR LF line ends, also where they had plain newlines
before, while the model sees plain newlines; `run_command` takes `cmd.exe`
syntax (`COMMAND.COM` on Windows 9x) and cannot be timed out or
interrupted; MCP servers are not available; the config file's permissions
are not checked. Text is converted between UTF-8, which the model speaks,
and the code pages of Windows: replies are shown in the console's code
page (a character it lacks becomes its nearest, or `?`), and what you
type, the task on the command line and the output of commands go to the
model as UTF-8. Files are not converted. Junctions and 8.3 short names are not resolved, so a
junction inside the working directory leads the file tools out of it.

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
