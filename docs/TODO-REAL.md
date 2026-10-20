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
| The 16-bit build (`dos16`) on an 8086, 8088 or 286 | QEMU's 486; DOSBox-X's 8086 and 286 (the network on its 286 only: its NE2000 loses the 8086's byte reads) | A real one: speed, the network on an 8086 or 8088 (an 8-bit card, NE1000), and 640K with real drivers loaded |
| The 32-bit build (`dos`) on a 386 or 486 | QEMU, emulated NE2000 | A real machine with a real packet driver; a 386 without a coprocessor |
| MS-DOS 2.11, 3.30, 4.01, 6.22, DR-DOS 7.03, FreeDOS 1.3 | QEMU, booted from the install floppy; MS-DOS 6.22 also with Windows 3.1's HIMEM and EMM386, the packet driver loaded high | An installed system with its usual drivers; QEMM |
| DOS boxes of Windows 1.04, 2.03, 2.11, 2.11/386, 3.0, 3.1 (3.x also in standard mode) | QEMU, VGA, no mouse | Real machines of the time; a network shared with Windows itself |
| SPX (`relay = spx:...`), both builds | PDIPX, IPXODI with ODIPKT (Ethernet II), and a WSGEN-made `IPX.COM` (raw 802.3), on QEMU's NE2000, wired straight to gork-relay | A real IPX network with other nodes, a NetWare server or a router on it; 802.2 and SNAP frames |

## Windows

| System | Tried in | Unproven |
|---|---|---|
| Windows 95, 95 OSR2, 98 SE, Me | QEMU, emulated Pentium or Pentium II | Real hardware, a real 486 above all: the programs hold no instruction newer than the 486, but none has run on one |
| Windows NT 3.1, 3.51, NT 4.0, 2000, XP | QEMU | Real hardware; service packs other than the ones tried |
| SPX from Windows | Windows Me in QEMU (`testing/winspxtest.py`) | Any other Windows; a real IPX network |
| The Cygwin package (`win32`) | GitHub's Windows Server runner: batch mode, and the TUI in a pseudo console | A desktop Windows 10 or 11; the TUI in Windows Terminal; MCP servers started from the package |

Windows 11 with the legacy build is the one Windows result from a real
machine.

## Unix

| System | Tried in | Unproven |
|---|---|---|
| Slackware 2.0.0 and 3.0, Debian 1.3 | QEMU | Hardware of the time: a 386 or 486 with 4 to 8 MB |
| Debian 2.1 and 2.2 | Docker, on a current kernel | Their own kernels |
| FreeBSD 2.0 and 2.0.5, NetBSD 1.0, OpenBSD 2.0 | QEMU | Real hardware |
| Dell Unix SVR4 Issue 2.2 | 86Box (a Pentium, WD8013EBT network card), over telnet; fake API on the same machine, and Qwen in LM Studio across the LAN | Real hardware of the time; the console |
| Solaris 2.5.1, 2.6 and 10, OpenSolaris 134, OmniOS | QEMU (x86) | SPARC, which is what most of these ran on |
| 9front (Plan 9), batch mode | QEMU, serial console; fake API on the same machine, and Qwen in LM Studio across the LAN | Real hardware; drawterm and rio |
| Minix 2.0.4 and 3.3.0, GNU/Hurd, DragonFly BSD, MidnightBSD | QEMU | Real hardware |
| OS/2 Warp 3 Connect and 4.52 | QEMU, booted to a text-mode `CMD.EXE` (no desktop), fake API and MCP server across QEMU's user network | Real hardware; a VIO window on the desktop; a FAT boot disk |
| NeXTSTEP 3.3 (Intel) | 86Box (an Adaptec 1542C for disk and CD, WD8013EBT network card), text console; fake API (`testing/fakeapi.c`) and MCP server on stdio over loopback: TCP to the host does not get through | Any network; HTTP MCP; real hardware; the arrow keys |
| OPENSTEP 4.2 (Intel) | 86Box (a Pentium on an i430HX board, PCnet-PCI network card), text console; fake API and MCP server across 86Box's SLiRP network | Real hardware; Terminal.app on the desktop; the arrow keys, which the emulated console does not send in a usable form |
| SunOS 4.1.4 | QEMU's SPARCstation 5 with its real PROM, serial console; fake API and MCP server across QEMU's user network | Real hardware; the Sun console and keyboard; sun4c machines |
| QNX 6.2.1 | QEMU (an ISA NE2000), in Photon's terminal; fake API and MCP server across QEMU's user network | Real hardware; the text console; QNX 6.3 and later |
| BeOS R5 | QEMU (Pentium II, RTL8139 network card), the Terminal app; fake API and MCP server across QEMU's user network | Real hardware; BONE; the TUI over telnet |
| AIX PS/2 1.3 | 86Box (PS/2 Model 80), against a fake API on the same machine over loopback | Any network at all. AIX PS/2's one Ethernet driver wants a 3Com 3C523 EtherLink/MC, which 86Box does not emulate; adding that card to 86Box would let the emulator test it |
| AIX 4.3.3 | QEMU's 40p (a patched QEMU 3.0; 11.1 does not boot AIX), over telnet; fake API on the same machine, MCP over HTTP across QEMU's user network | Real RS/6000 hardware; the round trip against a real model (LM Studio was down when it was tested) |
| HP-UX 10.20 and 11i v1 (11.11) | QEMU's B160L (PA-RISC 1.1), with gork cross-compiled to assembly on Linux and assembled and linked on HP-UX; fake API and MCP server across QEMU's user network | Real hardware; a native gcc build on PA-RISC 2.0; a real terminal instead of the serial console |
| Linux on s390x, armhf, arm64, ppc64el, riscv64, m68k, powerpc, hppa | QEMU's user-mode emulation: the suite only, on the host's kernel; m68k also natively, the whole suite, on Debian in QEMU's Quadra 800 | The machines themselves; the TUI on a real terminal |
| NetBSD 11.0 on m68k (virt68k) | QEMU's m68k `virt` machine, 256 MB (the kernel panics at boot with 1 GB), over ssh; fake API on the same machine, and Qwen in LM Studio across the LAN | A real 68k machine (mac68k, amiga, atari, hp300) |
| OpenBSD 7.9 on SPARC64 | QEMU, batch checks | The TUI; a real machine |
| Mac OS X 10.4 (PowerPC) | QEMU's mac99 (a G4), over ssh; fake API on the same machine, and Qwen in LM Studio across the LAN | Real PowerPC Macs; Terminal.app; Tiger on Intel |
| macOS | GitHub's runner: the suite | The TUI; Intel Macs; releases between 10.4 and 26 |

SuSE 8.1, Haiku, NetBSD 10.1 and openSUSE are the Unix results that did
not come from an emulator.

OPENSTEP 4.2, SunOS 4.1.4, QNX 6.2.1, BeOS R5, HP-UX 10.20, Debian m68k
and OS/2 Warp 4.52 (listed with the Unix systems above) have also done a
tool round trip (read_file, write_file, run_command) against a real model
instead of the fake: Qwen3.6-35B-A3B in LM Studio, on another machine on
the LAN, reached through the emulator's NAT.

## gork-relay

| Part | Tried in | Unproven |
|---|---|---|
| The `eth:` link | A tap device on Linux | A real network card on a real segment |
| On Cygwin, with Npcap | Not at all | Everything |
