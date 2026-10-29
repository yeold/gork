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
- [GNU/Hurd](#gnuhurd)
- [Haiku](#haiku)
- [macOS](#macos)
- [Cygwin](#cygwin)
- [No curses headers](#no-curses-headers)

Systems that build by hand, or with their own toolchain, have a guide
of their own:

- [AIX](build/aix.md) (4.3 on POWER, and AIX PS/2)
- [BeOS](build/beos.md)
- [DOS](build/dos.md)
- [HP-UX](build/hpux.md)
- [Minix](build/minix.md)
- [NeXTSTEP and OPENSTEP](build/nextstep.md)
- [OS/2](build/os2.md)
- [Plan 9](build/plan9.md)
- [QNX](build/qnx.md)
- [SunOS 4](build/sunos4.md)
- [System V Release 4.0](build/svr4.md)
- [UEFI](build/uefi.md) (`gork.efi`, no operating system)
- [Windows (native)](build/windows.md)

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
| `--with-max-file-size=BYTES` | Change the maximum file size `read_file`, `write_file` and `edit_file` accept |
| `--enable-unicode` / `--disable-unicode` | Force UTF-8 support in the TUI on or off (by default `configure` enables it when `ncursesw` and `locale.h` are present and falls back to ASCII otherwise) |
| `--without-tui` | Build without curses (batch mode only) |
| `--without-mcp` | Build without the MCP client |
| `--with-openssl[=DIR]` | Do TLS with OpenSSL (1.0.2 or later; under `DIR` if given), so that no relay is needed. Needs `libssl-dev` / `openssl-devel` |
| `--with-serial` | No network: everything over a serial line to `gork-relay -S` ([SERIAL.md](SERIAL.md)). Needs only `termios.h` |
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

Releases carry static binaries for FreeBSD 15.1 on amd64 and arm64,
`gork-<version>-freebsd-<arch>.tar.gz`, cross-built on Linux by
`bsdbuild.sh` (clang and lld, against the release's own `base.txz`); the
same script makes the NetBSD and OpenBSD ones below.

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

Releases carry static binaries for NetBSD 11.0 on amd64 and arm64,
`gork-<version>-netbsd-<arch>.tar.gz`, from `bsdbuild.sh`.

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

Releases carry static binaries for OpenBSD 7.9 on amd64 and arm64,
`gork-<version>-openbsd-<arch>.tar.gz`, from `bsdbuild.sh`. OpenBSD keeps
no system call interface stable between releases, so they are for 7.9
only; on another release, build from the tarball.

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

**Mac OS X 10.4** (PowerPC, gcc 4.0 from the Xcode Tools on the install
DVD, ncurses 5.4 with the system) builds without `configure`:

```sh
cc -O2 -o gork main.c relay.c tui.c mcp.c cJSON/cJSON.c -lncurses -lm
```

Batch mode, the TUI (over ssh) and MCP on stdio and over HTTP work,
tested on QEMU's `mac99` machine (a G4) with the 10.4.0 retail DVD. Its
OpenSSH 3.8 server wants old key exchanges and a 1024-bit RSA host key,
which today's ssh client accepts only when told to.

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
([Windows](build/windows.md)): it runs on everything back to Windows 95, and has
no MCP.

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
