# Wanted: a run on the real thing

These systems work, but only in an emulator or on a CI runner, and
against a fake API on the same machine. Nobody has run gork on them the
way it is meant to be used: real hardware, a real network card, a relay
on another machine and a real model behind it. A report from any of them
is welcome ([CONTRIBUTING.md](../CONTRIBUTING.md) says what to include).

What an emulator does not show: timing (QEMU is far faster or slower than
the machine), real network drivers, memory as tight as it really was,
terminals and keyboards, and CPUs that QEMU only pretends to be: its
"486" runs Pentium Pro instructions.

## DOS

| System | Tried in | Unproven |
|---|---|---|
| The 16-bit build (`dos16`) on an 8086, 8088 or 286 | QEMU's 486 only | That it starts at all below a 386: it is compiled for the 8086 and never met one. Speed, and 640K with real drivers loaded |
| The 32-bit build (`dos`) on a 386 or 486 | QEMU, emulated NE2000 | A real machine with a real packet driver; a 386 without a coprocessor |
| MS-DOS 3.30, 4.01, 6.22, DR-DOS 7.03, FreeDOS 1.3 | QEMU, booted from the install floppy | An installed system with its usual drivers and memory managers (EMM386, QEMM) |
| DOS boxes of Windows 1.04, 2.03, 2.11, 2.11/386, 3.0, 3.1 | QEMU, VGA, no mouse | Real machines of the time; Windows 3.x standard mode; a network shared with Windows itself |
| SPX (`relay = spx:...`), both builds | PDIPX on QEMU's NE2000, wired straight to gork-relay | `IPXODI` and `IPX.COM`; a real IPX network with other nodes, a NetWare server or a router on it; frame types other than Ethernet II |

## Windows

| System | Tried in | Unproven |
|---|---|---|
| Windows 95, 95 OSR2, 98 SE, Me | QEMU, emulated Pentium or Pentium II | Real hardware. A real 486: two Pentium Pro instructions remain in MinGW's start-up code, and QEMU does not refuse them |
| Windows NT 3.51, NT 4.0, 2000, XP | QEMU | Real hardware; service packs other than the ones tried |
| SPX from Windows | Windows Me in QEMU, by hand | Any other Windows; a real IPX network |
| The Cygwin package (`win32`) | GitHub's Windows Server runner, batch mode | A desktop Windows 10 or 11; the TUI, in the console and in Windows Terminal; MCP servers started from the package |

Windows 11 with the legacy build is the one Windows result from a real
machine.

## Unix

| System | Tried in | Unproven |
|---|---|---|
| Slackware 2.0.0 and 3.0, Debian 1.3 | QEMU | Hardware of the time: a 386 or 486 with 4 to 8 MB |
| Debian 2.1 and 2.2 | Docker, on a current kernel | Their own kernels |
| FreeBSD 2.0 and 2.0.5, NetBSD 1.0, OpenBSD 2.0 | QEMU | Real hardware |
| Solaris 2.5.1, 2.6 and 10, OpenSolaris 134, OmniOS | QEMU (x86) | SPARC, which is what most of these ran on |
| Minix 3.3.0, GNU/Hurd, DragonFly BSD, MidnightBSD | QEMU | Real hardware |
| Linux on s390x, armhf, arm64, ppc64el, riscv64, m68k, powerpc, hppa | QEMU's user-mode emulation: the suite only, on the host's kernel | The machines themselves, and the TUI on them |
| OpenBSD 7.9 on SPARC64 | QEMU, batch checks | The TUI; a real machine |
| macOS | GitHub's runner: the suite | The TUI; Intel Macs; older releases |

SuSE 8.1, Haiku, NetBSD 10.1 and openSUSE are the Unix results that did
not come from an emulator.

## gork-relay

| Part | Tried in | Unproven |
|---|---|---|
| The `eth:` link | A tap device on Linux | A real network card on a real segment |
| On Cygwin, with Npcap | Not at all | Everything |
