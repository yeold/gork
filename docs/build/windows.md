# Building gork on Windows (native)

Back to [all systems](../BUILD.md).

Experimental. Besides the [Cygwin](../BUILD.md#cygwin) build there is a native one:
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

It is compiled for the 486, and `winbuild.sh` checks that no instruction
newer than that is left in either program (MinGW's start-up code brought
two, which `win.c` now keeps out). It has still only run on emulated
processors, never on a real 486.

On Linux, `winbuild.sh` downloads the PDCurses source and leaves
`gork-<version>-win32-legacy.zip` in `out/` ("legacy" since the
[Cygwin package](../BUILD.md#cygwin) took the plain name; this build has no MCP: it
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
