# TODO: operating systems still to test

None of these has been tried. What works is in the
[README table](../README.md#tested-systems), and what works only in an
emulator so far in [TODO-REAL.md](TODO-REAL.md). The notes are expectations, from what gork
needs (a C89 compiler, curses, BSD sockets, `fork`/`pipe`/`select`,
`realpath`) and from what broke on the systems that work. Grouped by
expected effort.

Only major versions belong here, and versions that differ in something
gork depends on: when one release of a line works, its point releases
are taken to work too.

## Unix: probably `configure`, `make` and small fixes

| OS | How to run it | What to expect |
|---|---|---|
| AIX on POWER (4.3, 5.x, 7.x) | QEMU `ppc64` runs AIX 7.2; older wants real RS/6000 hardware | System V curses as on Solaris. Needs gcc from the AIX Toolbox, or `xlc`. Sockets are in libc. AIX PS/2 1.3 works (README); its fixes are under `GORK_AIXPS2`, which needs `_I386`, so they stay off here. |
| HP-UX 11i v3 | A PA-RISC 2.0 or Itanium machine | 10.20 and 11.11 work, cross-compiled (README). |
| IRIX (5.3, 6.5) | No good emulator; MAME's Indy boots but is slow. Real SGI hardware is the practical route | System V curses, MIPSpro `cc` or gcc. Should be close to Solaris. |
| Tru64 / OSF/1 | Alpha emulation in QEMU is limited; real hardware | Similar to the other System V systems. |
| UnixWare, SCO OpenServer | x86 in QEMU | SVR4 and SVR3 curses; old SCO needs `-lsocket`, which `configure` already looks for. |
| Ultrix, A/UX | SIMH (VAX Ultrix), Shoebill (A/UX) | Pre-ANSI bundled compilers, so gcc, as on SunOS 4 (see docs/BUILD.md). |
| m68k Macintosh, with a Unix on it | QEMU's `q800` machine (a Quadra 800) runs Linux/m68k and NetBSD/mac68k, and newer QEMU is said to boot A/UX 3; Shoebill runs A/UX on an emulated Mac II. Not checked which of these work here | Linux/m68k (Debian ports) and NetBSD/mac68k should be `configure` and `make`; so far m68k has run only the suite, under user-mode emulation. A/UX is System V with BSD sockets and a pre-ANSI `cc`, so it needs gcc, as SunOS 4 did. The classic Mac OS on the same machines is a port: see below. |
| NeXTSTEP 3.3 | 86Box for Intel, as OPENSTEP 4.2; Previous for the 68k NeXT machines | OPENSTEP 4.2 works (README). 3.3 has an older NeXT cc (gcc 2.5-based) and the same 4.3BSD library, so expect the OPENSTEP fixes in `nextstep.c` and ncurses 4.2 to carry over. |
| Mac OS X (old) | `qemu-system-ppc` for 10.4 and 10.5 | Should behave like FreeBSD. |
| QNX 4 | x86 in QEMU | QNX 6.2.1 works (README). QNX 4 is a different system underneath, with Watcom C. |
| Linux with dietlibc | A cross toolchain | No curses port in common use; batch mode only, if at all. |

## Not Unix, but a POSIX layer exists

| OS | Route | What to expect |
|---|---|---|
| AmigaOS, MorphOS, AROS | ixemul or libnix with gcc; UAE, or AROS in QEMU | `fork` is emulated badly or not at all; sockets come from `bsdsocket.library`. |
| z/OS (UNIX System Services) | No emulator route that is both free and licensed: Hercules runs z/OS, but only with IBM's media (ZD&T, a paid licence). IBM Z Xplore gives free shell accounts on a real z/OS; whether a compiler and outbound TCP are allowed there is the first thing to find out. The public-domain MVS 3.8j (TK4-, TK5) has no USS and is a different job. | USS is a certified Unix: `fork`, BSD sockets, `select`, `realpath` and a curses library are there, and the compiler is `xlc`/`c89` or `ibm-clang`. The wall is EBCDIC: character constants in the source become EBCDIC, while the HTTP headers and the JSON on the wire are ASCII, so gork must be built in ASCII mode (`-qascii` with `xlc`, `-fzos-le-char-mode=ascii` with `ibm-clang`) and its files tagged, or every literal compared against the network breaks. Expect curses and the terminal (3270 or an ssh pty, in EBCDIC) to be the next problem; start with batch mode. |
| OpenVMS | SIMH VAX, Alpha emulators, x86 in a VM | DEC C has sockets and curses, but only `vfork`, and paths are not Unix paths. |
| Atari MiNT | Hatari or ARAnyM | A POSIX layer with gcc; probably feasible. |

## A port, not a build

| OS | What it would take |
|---|---|
| Plan 9, 9front | APE gives a C89 compiler, `fork` and BSD-style sockets, but there is no curses and no termios. Batch mode is plausible through APE; the TUI would have to be rewritten for the Plan 9 console. Runs well in QEMU. |
| UEFI application (`gork.efi`) | No operating system at all: a PE executable started from the UEFI shell or boot menu, 64-bit, with all the RAM. Firmware offers TCP4/TCP6, DHCP and DNS protocols, a text console and FAT file access, so the pieces exist. No libc (EDK2's StdLib is a partial one with a socket layer, or write thin replacements), no curses, no `fork` and no signals. Expect to rewrite the transport against the event-based TCP protocol, give the TUI its own screen layer, write `realpath` for `fs0:\dir\file` paths, and leave out `run_command` and MCP. The DOS port is the model: no `fork`, polled timeout, odd path syntax. Test in QEMU with OVMF; start with batch mode. Real firmware often has the network stack off except for PXE. |
| U-Boot and other boot firmware (coreboot payload, GRUB module, barebox, Open Firmware) | Same idea as UEFI, with less underneath. First question for each is whether it has TCP at all: boot loaders traditionally stop at UDP (TFTP, DHCP). U-Boot: a "standalone application" gets only a small jump table (console, malloc, timers), but newer builds carry an lwIP stack with TCP (`wget`), and U-Boot can also start UEFI applications, so `gork.efi` may run there if its UEFI subset includes the TCP protocols. coreboot: a payload linked against libpayload gets a small libc and a curses, but no network stack. GRUB: has TCP and HTTP in its `net` module, but code must be a GRUB module. barebox: Linux-like shell and drivers, network mostly UDP. Open Firmware: Forth and a client interface; TCP is rare. Nothing here has been checked; find out what each offers before planning. Test in QEMU (`-bios u-boot.bin`, coreboot ROM, OVMF). |
| Classic Mac OS on m68k: System 6 (6.0.x) and System 7 (7.0 to 7.6; 8.0 and 8.1 likely the same) | No Unix underneath: no processes, no `fork`, no command line and no curses. TCP comes from MacTCP, the only stack System 6 has (Open Transport needs 7.5 or later); neither is BSD sockets, though GUSI wraps them as such (GUSI 1 for MacTCP, GUSI 2 needs Open Transport). The Retro68 cross toolchain (gcc for m68k and PowerPC Macs) has a small console window library, enough for batch mode; the TUI needs its own screen layer, and `run_command` and MCP go, as on DOS. Paths use `:`. System 6 means a 68000 (Mac Plus, SE) with 1 to 4 MB, no colour and 32 KB code segments, so expect the DOS 16-bit limits; System 7 adds 32-bit addressing on a 68020 or better. Test System 7 in QEMU `q800` (7.1 and later) or Basilisk II, both of which can give the guest a network. For System 6 that is the open question: Mini vMac runs it but has no network, and Basilisk II starts at System 7; MAME's Mac II or SE/30 may do it, unchecked. All of them need a Mac ROM image. Find out first whether Retro68 still targets System 6, and whether its libc and GUSI are enough for `relay.c`. |
| Visopsys | Its own C library and process model. Find out first whether user programs get sockets; expect to rewrite transport, command execution and the TUI. |
| ZealOS, Shrine | The TempleOS forks that add a TCP/IP stack (both are meant to run in a VM with an emulated PCnet card), which plain TempleOS lacks by design. Programs are still HolyC (ZealC on ZealOS), not C, with no libc, no curses and no processes in the Unix sense: the transport could use their socket calls, but the source would have to be translated and the TUI redrawn with their text output. Check first which of the two has the more complete TCP client API. |
| Palm OS (3.x to 5.x) | No processes, no console and no curses: a Palm OS application is an event loop drawing forms. TCP comes from NetLib (Palm OS 3 and up), which is close to BSD sockets but not the same, and `select` exists as `NetLibSelect`. Build with prc-tools (gcc for m68k) or CodeWarrior; Palm OS 5 runs m68k code on ARM through PACE. The TUI becomes a form with a field and a scrolling text area; `run_command` and MCP go, as on DOS. Test in POSE (Palm OS 3.x/4.x ROMs, which can pass NetLib calls to the host's sockets) or the Palm OS 5 Simulator. Memory (a few hundred KB of dynamic heap) and the 32 KB code segment limit are the first walls. |
| Windows Mobile / Pocket PC (Windows CE 3 to 6) | Winsock is there, so `relay.c` should mostly carry over from the Windows build, but there is no console on Pocket PC (third-party PocketConsole adds one), no current directory, no environment variables, and Unicode-only Win32 calls. Build with eMbedded Visual C++ 3/4 or CeGCC (`mingw32ce`). Test in Microsoft's Device Emulator (ARM) or the Pocket PC 2002/2003 emulators (x86), which run on Windows. Batch mode in PocketConsole first; a real UI would be a dialog with an edit box. |
| CP/M, Atari TOS, RISC OS | Harder still: no process model or no sockets that gork could use as it is. |

## More systems with a TCP/IP stack

gork does HTTP itself, so all it needs from the system is TCP sockets
reachable from C. These have them and are not listed above.

| OS | Notes |
|---|---|
| GhostBSD | A FreeBSD desktop distribution; expected to behave like FreeBSD. |
| Darwin / PureDarwin, iOS (jailbroken), Android (Termux, bionic libc) | Unix underneath; Termux has clang and ncurses packages. |
| SerenityOS, Redox, Managarm, ToaruOS, Sortix | Hobby Unix-likes with a POSIX libc, sockets and an ncurses port; each is a small-libc test. |
| ReactOS | Aims at NT compatibility; a native Windows build should run there. |
| eComStation, ArcaOS | OS/2 descendants; the EMX build of Warp 3 and 4.52 should run, and kLIBC is the newer route. |
| Xenix, 386BSD, BSD/OS, 4.3BSD on VAX (SIMH) | Old Unix with TCP/IP. Expect the `realpath` wall that stopped FreeBSD 1.x on the Net/2-era ones. |
| Coherent | Open source since 2015, so the media is free, but it has no TCP/IP of its own: nothing for gork to connect with. |
| Domain/OS, DG/UX, Dynix/ptx, NEWS-OS, UNICOS, Sinix | Vendor Unix; media and emulators are the obstacle, not the code. |
| VxWorks, RTEMS, LynxOS, eCos, NuttX, Zephyr | Real-time systems with BSD-style sockets and a C toolchain, but no `fork` and usually no curses: batch mode at most. |
| IBM i PASE, Stratus VOS, HP NonStop OSS | Mainframe and midrange POSIX environments. (z/OS has its own row above.) |
| GEOS | Has TCP/IP and some sockets layer, but no process model gork can use as it is. |
| MenuetOS, KolibriOS, Genode, Fuchsia, seL4-based systems | Networked, but their own APIs: a rewrite of transport and TUI. |

## Suggested order

1. Mac OS X 10.4 on PowerPC, in QEMU: an old, big-endian macOS.
2. Linux/m68k or NetBSD/mac68k on QEMU's `q800`: m68k on a whole machine,
   TUI included.
3. NeXTSTEP 3.3: reuses the OPENSTEP 4.2 setup.
4. Plan 9: batch mode only as a first goal.
5. UEFI application: reuse the decisions of the DOS and Windows ports
   (no `fork`, a polled timeout, odd path syntax); batch mode in QEMU with
   OVMF first. Then U-Boot and the other boot firmware: check each for TCP
   first; U-Boot running `gork.efi` is the cheapest thing to try.
6. Classic Mac OS (System 7 first, in `q800` or Basilisk II), then
   System 6 if an emulator with a network for it turns up.

Set aside for now: AIX on POWER (AIX 5.1 in QEMU has no working network;
AIX 7.2 media is IBM's licensed software), ArcaOS (a current commercial
product), IRIX (hardware or a slow MAME).
