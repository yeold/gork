# TODO: operating systems still to test

None of these works yet, though some rows say what was tried. What works is in the
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
| AIX on POWER (3.x, 5.x, 7.x) | QEMU's `40p` (PReP) runs 5.1 without a network (its network driver fails); `ppc64` runs AIX 7.2; 3.x wants real RS/6000 hardware. WinWorld has 3.x to 5.1 | AIX 4.3.3 and AIX PS/2 1.3 work (README), 4.3 with no source change. Needs gcc, or `xlc`. |
| HP-UX 11i v3 | A PA-RISC 2.0 or Itanium machine | 10.20 and 11.11 work, cross-compiled (README). |
| IRIX (5.3, 6.5) | No good emulator; MAME's Indy boots but is slow. Real SGI hardware is the practical route | System V curses, MIPSpro `cc` or gcc. Should be close to Solaris. |
| DEC OSF/1, Digital UNIX, Tru64 (2.x to 5.1B, all on WinWorld) | QEMU's Alpha runs only Linux (no SRM console). The es40 emulator boots Tru64 but is slow and finicky; FreeAXP and AlphaVM run it, on Windows only; real hardware | Sockets in libc, a system curses, DEC's ANSI C or gcc. 64-bit (LP64), as Linux and the BSDs already are, so likely a small `port.h` block at most. The emulator is the hard part. |
| UnixWare, SCO OpenServer | 86Box: UnixWare 2.1's install floppy boots there (QEMU's BIOS will not boot it, and with that fixed its kernel panics). The 2.1.2 Free Edition's installer then asks for a serial number and activation key, which SCO gave out per user | SVR4 and SVR3 curses; old SCO needs `-lsocket`, which `configure` already looks for. |
| Ultrix, A/UX 1.x and 2.x | GXemul's DECstation (MIPS Ultrix 4.x, with a network) or SIMH (VAX Ultrix), Shoebill (A/UX, fragile) | Pre-ANSI bundled compilers, so gcc, as on SunOS 4 (see [docs/build/sunos4.md](build/sunos4.md)). |
| m68k Macintosh, NetBSD/mac68k or A/UX | QEMU's `q800` machine (a Quadra 800) runs NetBSD/mac68k, though only through its Mac OS booter (a Mac ROM and System 7), and newer QEMU is said to boot A/UX 3; Shoebill runs A/UX on an emulated Mac II | Debian on the same `q800` works, and NetBSD 11.0 on m68k works as virt68k (README), so NetBSD/mac68k should too. A/UX is System V with BSD sockets and a pre-ANSI `cc`, so it needs gcc, as SunOS 4 did. The classic Mac OS on the same machines is a port: see below. |
| Mac OS X (old): 10.0 to 10.3, and Mac OS X Server 1.0 (Rhapsody) | `qemu-system-ppc`; 10.0 and Rhapsody boot less reliably in it | 10.4 on PowerPC works (README), with no change to the source. gcc comes with the Developer Tools. |
| SVR4 on the 386: AT&T System V, Intel Unix System V, ESIX | x86 in QEMU or 86Box (floppy installs) | Dell Unix SVR4 works (README). ESIX 4.0.3 on archive.org lacks base disk 10; AT&T's 2.1 on WinWorld has no C compiler or TCP/IP. |
| NeXTSTEP 0.x to 2.x | Previous (68k NeXT machines, with a network) | 68k only. Likely close to OPENSTEP 4.2, with an older NeXT cc. |
| QNX 4 | x86 in QEMU: the 4.25 CD (WinWorld) boots there and its installer runs, but its product installer wants a licence number for every product, the OS included, and installs nothing without one | QNX 6.2.1 works (README). QNX 4 is a different system underneath, with Watcom C 10.6 and a TCP/IP toolkit, both on the same CD. |
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
| Plan 9: the TUI; Bell Labs' 4th edition | 9front works in batch mode (README). A TUI would have to be written for the Plan 9 console or for rio, since there is no curses. The 4th edition's APE is older: expect more gaps than 9front's. |
| UEFI: the TUI | gork works on x86-64 and arm64 UEFI, in batch mode (README). A TUI wants a screen layer of its own on the text console (cursor and colours through SimpleTextOutput, keys through SimpleTextInputEx), as there is no curses. Real PCs: see [TODO-REAL.md](TODO-REAL.md). |
| Other boot firmware (coreboot payload, GRUB on BIOS, Open Firmware) | U-Boot works, through its UEFI, and barebox and GRUB on UEFI as a command on gork's own TCP/IP (README). For the rest the first question is whether they have TCP at all: boot loaders traditionally stop at UDP (TFTP, DHCP), though gork's own TCP/IP (`ip.c`) needs only a way to send and receive Ethernet frames. coreboot: a payload linked against libpayload gets a small libc and a curses, but no network stack. GRUB on a BIOS PC (`i386-pc`): its one network driver is the PXE ROM's, so only a machine booted from the network has a card; and gork's half wants the FPU without SSE, as GRUB builds 32-bit code. Open Firmware: Forth and a client interface; TCP is rare. Test in QEMU (coreboot ROM). |
| Classic Mac OS on m68k: System 6 (6.0.x) and System 7 (7.0 to 7.6; 8.0 and 8.1 likely the same) | Builds (`macbuild.sh`, docs/build/mac.md), not yet run: System 7, batch mode in a console window, MacTCP for the network, as the barebox and GRUB builds are put together; testing waits for a Mac ROM. No Unix underneath: no processes, no `fork`, no command line and no curses. TCP comes from MacTCP, the only stack System 6 has (Open Transport needs 7.5 or later); neither is BSD sockets, though GUSI wraps them as such (GUSI 1 for MacTCP, GUSI 2 needs Open Transport). The Retro68 cross toolchain (gcc for m68k and PowerPC Macs) has a small console window library, enough for batch mode; the TUI needs its own screen layer, and `run_command` and MCP go, as on DOS. Paths use `:`. System 6 means a 68000 (Mac Plus, SE) with 1 to 4 MB, no colour and 32 KB code segments, so expect the DOS 16-bit limits; System 7 adds 32-bit addressing on a 68020 or better. Test System 7 in QEMU `q800` (7.1 and later) or Basilisk II, both of which can give the guest a network. For System 6 that is the open question: Mini vMac runs it but has no network, and Basilisk II starts at System 7; MAME's Mac II or SE/30 may do it, unchecked. All of them need a Mac ROM image. Find out first whether Retro68 still targets System 6, and whether its libc and GUSI are enough for `relay.c`. |
| KolibriOS | A small x86 system written in assembly (FASM), with its own TCP/IP stack and drivers for the cards QEMU emulates (RTL8139, PCnet, e1000). No Unix underneath: no `fork`, no curses, and programs talk to the kernel through its system calls and shared libraries (`network.obj`, `console.obj`). C is possible: the system has a port of TinyCC (`ktcc`) with a small libc, and a gcc cross toolchain with newlib exists. Expect the DOS port's shape: batch mode first, on the console library; the transport rewritten against its socket calls (or a socket layer in its libc, if there is one); `realpath` for its `/sys/...` and `/hd0/1/...` paths; no `run_command` and no MCP to start with. Check first, in QEMU: that the network comes up, which C route compiles cJSON and `relay.c`, and whether its libc has BSD-style sockets. |
| Visopsys | Its own C library and process model. Find out first whether user programs get sockets; expect to rewrite transport, command execution and the TUI. |
| ZealOS, Shrine | The TempleOS forks that add a TCP/IP stack (both are meant to run in a VM with an emulated PCnet card), which plain TempleOS lacks by design. Programs are HolyC (ZealC on ZealOS), compiled by the system itself, not C: no libc, no curses, and everything in one address space in ring 0, so even batch mode has the whole machine. None of the source carries over, cJSON included; this is a separate program that speaks gork's protocol, not a build of gork. First goal: a HolyC client that POSTs a task to `/v1/messages` (plain HTTP to a relay), prints the reply, and runs `read_file`, `write_file` and `list_dir` on the system's own file system, with a small JSON reader; no `run_command` and no MCP, as on DOS. Before writing it: boot ZealOS in QEMU with a PCnet card and see that it gets an address, find which of the two has the more usable TCP client API, and POST to `testing/fakeapi.c` from a few lines of HolyC. |
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
| Coherent, PC/IX, Venix 386, early Xenix (8086, 286, IBM PC 1.0 and 2.0) | No TCP/IP of their own: nothing for gork to connect with. Coherent is open source since 2015, so its media is free. |
| Inferno | Programs are Limbo, not C, on its own virtual machine: a rewrite, not a port. |
| Domain/OS, DG/UX, Dynix/ptx, NEWS-OS, UNICOS, Sinix | Vendor Unix; media and emulators are the obstacle, not the code. |
| VxWorks, RTEMS, LynxOS, eCos, NuttX, Zephyr | Real-time systems with BSD-style sockets and a C toolchain, but no `fork` and usually no curses: batch mode at most. |
| IBM i PASE, Stratus VOS, HP NonStop OSS | Mainframe and midrange POSIX environments. (z/OS has its own row above.) |
| GEOS | Has TCP/IP and some sockets layer, but no process model gork can use as it is. |
| MenuetOS, Genode, Fuchsia, seL4-based systems | Networked, but their own APIs: a rewrite of transport and TUI. |

## Suggested order

1. The other boot firmware: barebox and GRUB on UEFI work (README).
   coreboot's libpayload has no network driver and no file system to
   write to; GRUB on BIOS has only the PXE ROM's driver. Check each for a
   way to send Ethernet frames first; `ip.c` does the rest.
2. Classic Mac OS (System 7 first, in `q800` or Basilisk II), then
   System 6 if an emulator with a network for it turns up.
3. Tru64 on Alpha, in es40: try a networked install first; the port
   itself should be small.
4. ZealOS or Shrine: a small HolyC client rather than a port; check the
   network and the TCP client API in QEMU first.

Set aside for now: AIX on POWER (AIX 5.1 in QEMU has no working network;
AIX 7.2 media is IBM's licensed software), ArcaOS (a current commercial
product), IRIX (hardware or a slow MAME), UnixWare (a licence key for
the Free Edition), QNX 4 (a licence number for each product).
