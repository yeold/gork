# TODO: operating systems still to test

None of these has been tried. The notes are expectations, from what gork
needs (a C89 compiler, curses, BSD sockets, `fork`/`pipe`/`select`,
`realpath`) and from what broke on the systems in the
[README table](../README.md#tested-systems). Grouped by expected effort.

## Unix: probably `configure`, `make` and small fixes

| OS | How to run it | What to expect |
|---|---|---|
| AIX (4.3, 5.x, 7.x) | QEMU `ppc64` runs AIX 7.2; older wants real RS/6000 hardware | System V curses as on Solaris. Needs gcc from the AIX Toolbox, or `xlc`. Sockets are in libc. |
| HP-UX (10.20, 11.x) | QEMU `hppa` runs 10.20 to 11.11; Itanium needs real hardware | System V curses (`-lcurses`, `-lHcurses` on old releases). The bundled `cc` is K&R only, so gcc is needed. |
| IRIX (5.3, 6.5) | No good emulator; MAME's Indy boots but is slow. Real SGI hardware is the practical route | System V curses, MIPSpro `cc` or gcc. Should be close to Solaris. |
| Minix (2.0, 3.x) | x86 in QEMU | Minix 3 has NetBSD's userland, so probably easy. Minix 2 has its own small libc: check `select`, `realpath` and curses first. |
| Tru64 / OSF/1 | Alpha emulation in QEMU is limited; real hardware | Similar to the other System V systems. |
| UnixWare, SCO OpenServer | x86 in QEMU | SVR4 and SVR3 curses; old SCO needs `-lsocket`, which `configure` already looks for. |
| NeXTSTEP / OPENSTEP | x86 in QEMU, or the Previous emulator for 68k | 4.3BSD base with BSD curses only, so ncurses must be added as on NetBSD 1.0. |
| SunOS 4, Ultrix, A/UX | QEMU `sparc` (SunOS 4.1.4), SIMH (VAX Ultrix), Shoebill (A/UX) | Pre-ANSI bundled compilers, so gcc. SunOS 4's libc lacks `strerror` and `memmove`. |
| macOS, Mac OS X | GitHub Actions for current releases; `qemu-system-ppc` for 10.4 and 10.5 | Should behave like FreeBSD. |
| QNX (4, 6) | x86 in QEMU | QNX 6 is POSIX with ncurses; QNX 4 uses Watcom C. |
| GNU/Hurd | Debian GNU/Hurd image in QEMU | No `PATH_MAX`, which `main.c` uses throughout. |
| Linux with musl, uClibc, dietlibc | Alpine container, or a cross toolchain | Most likely fine; cheap to check. |

## Not Unix, but a POSIX layer exists

| OS | Route | What to expect |
|---|---|---|
| OS/2 (Warp 3 and 4, ArcaOS) | EMX with gcc, or kLIBC; runs in QEMU | EMX has `fork`, pipes, sockets and a curses port, so it is nearer to Unix than DOS is. |
| Windows NT (3.51, 4.0, 2000, XP, current) | Cygwin, or native with MinGW, PDCurses and Winsock | Cygwin should mostly just work. Native needs the same work as DOS: no `fork`, `CreateProcess` for commands, Winsock start-up, backslash paths. |
| Windows 9x | The DJGPP `gork.exe` in a DOS box, or MinGW with PDCurses and Winsock | The DOS binary gets long file names there for free, but Watt-32 needs a packet driver shim. A Winsock build is the cleaner route. |
| BeOS R5 | x86 in QEMU | Haiku already works. BeOS has gcc 2.95 and sockets that are not file descriptors, so `select` on them is the risk. |
| AmigaOS, MorphOS, AROS | ixemul or libnix with gcc; UAE, or AROS in QEMU | `fork` is emulated badly or not at all; sockets come from `bsdsocket.library`. |
| OpenVMS | SIMH VAX, Alpha emulators, x86 in a VM | DEC C has sockets and curses, but only `vfork`, and paths are not Unix paths. |
| Atari MiNT | Hatari or ARAnyM | A POSIX layer with gcc; probably feasible. |

## A port, not a build

| OS | What it would take |
|---|---|
| Plan 9, 9front | APE gives a C89 compiler, `fork` and BSD-style sockets, but there is no curses and no termios. Batch mode is plausible through APE; the TUI would have to be rewritten for the Plan 9 console. Runs well in QEMU. |
| Visopsys | Its own C library and process model. Find out first whether user programs get sockets; expect to rewrite transport, command execution and the TUI. |
| TempleOS | No networking, by design, and programs are HolyC, not C. A relay over the serial port is the only plausible transport, and the source would have to be translated. A stunt rather than a target. |
| CP/M, classic Mac OS, Atari TOS, RISC OS | Same territory as TempleOS: no process model or no sockets that gork could use as it is. |

## More systems with a TCP/IP stack

gork does HTTP itself, so all it needs from the system is TCP sockets
reachable from C. These have them and are not listed above.

| OS | Notes |
|---|---|
| DragonFly BSD, MidnightBSD, GhostBSD | FreeBSD relatives; expected to behave like FreeBSD. |
| Darwin / PureDarwin, iOS (jailbroken), Android (Termux, bionic libc) | Unix underneath; Termux has clang and ncurses packages. |
| SerenityOS, Redox, Managarm, ToaruOS, Sortix | Hobby Unix-likes with a POSIX libc, sockets and an ncurses port; each is a small-libc test. |
| ReactOS | Aims at NT compatibility; a native Windows build should run there. |
| eComStation, ArcaOS | OS/2 descendants; same EMX/kLIBC route as OS/2. |
| Xenix, Coherent, 386BSD, BSD/OS, 4.3BSD on VAX (SIMH) | Old Unix with TCP/IP. Expect the `realpath` wall that stopped FreeBSD 1.x on the Net/2-era ones. |
| Domain/OS, DG/UX, Dynix/ptx, NEWS-OS, UNICOS, Sinix | Vendor Unix; media and emulators are the obstacle, not the code. |
| VxWorks, RTEMS, LynxOS, eCos, NuttX, Zephyr | Real-time systems with BSD-style sockets and a C toolchain, but no `fork` and usually no curses: batch mode at most. |
| z/OS UNIX System Services, IBM i PASE, Stratus VOS, HP NonStop OSS | Mainframe and midrange POSIX environments. EBCDIC on z/OS would break the JSON and HTTP text handling. |
| RISC OS, classic Mac OS (MacTCP/Open Transport, GUSI), GEOS | Have TCP/IP and some sockets layer, but no process model gork can use as it is. |
| MenuetOS, KolibriOS, Genode, Fuchsia, seL4-based systems | Networked, but their own APIs: a rewrite of transport and TUI. |

## Suggested order

1. Minix 3, macOS, Cygwin, musl: cheap, and likely to pass unchanged.
2. AIX and HP-UX in QEMU: System V variety beyond Solaris, and both emulate.
3. OS/2 with EMX, then native Windows (NT and 9x): the Windows work overlaps
   the DOS port (branch `dos`, `docs/TODO-DOS.md` there).
4. IRIX, NeXTSTEP, SunOS 4, Minix 2: these depend on hardware or slow
   emulators.
5. Plan 9: batch mode only as a first goal.
6. Visopsys, TempleOS: only for their own sake.
