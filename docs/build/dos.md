# Building gork on DOS

Back to [all systems](../BUILD.md).

Experimental; [TODO-DOS.md](../TODO-DOS.md) has what is left.
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
(`GORK_DOS_SMALL_DISKS=1` as well for MS-DOS 3.30, 4.01 and DR-DOS 7.03,
which read only disks laid out the old way, and `GORK_DOS_FAT12=1` instead for MS-DOS
2).

**Under Windows 3.0 and 3.1** the DOS `gork.exe` runs in a DOS box, in 386
enhanced mode: Windows provides the protected mode there, so `CWSDPMI.EXE`
is not used. It passes the same ten checks (`GORK_DOS_WINDOWS` names an
installed `WINDOWS` directory for `dostest.py`). Load the packet driver
before Windows, and `WINPKT` after it (`WINPKT 0x60`, from the same Crynwr
collection): without `WINPKT`, gork dies at its first disk access during a
request and the network is dead afterwards. The Windows `gork.exe` is of
no use here: Windows 3.x cannot run 32-bit console programs, with or
without Win32s. In standard mode, a 286's, use the 16-bit build: it
passes the ten checks under 3.0 and 3.1 there (no `WINPKT`), while this
one's `run_command` fails.

**Under Windows 1 and 2** it passes the ten checks as well: Windows 1.04
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
and `dostest.py` checks it when `GORK_RELAY` names a relay binary. With
ODI, TCP/IP on the same card goes through ODIPKT, both on the Ethernet_II
frame type; for the 16-bit build load the drivers high (`LH`), or
`run_command` finds no room for `COMMAND.COM`.

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
- Watt-32 is built without its optional parts, DHCP excepted (`my_ip =
  dhcp` in `WATTCP.CFG`; 16K): no BSD sockets, gork uses its own calls
  there.
- Before a command runs, the unused heap goes back to DOS (`_heapmin()`):
  FreeDOS without an XMS driver leaves 496K, of which gork takes 300K,
  and its command interpreter did not fit beside it otherwise.
- The linker must not pack code segments into groups
  (`option packcode=1`), or Watt-32's interrupt code crashes at
  `sock_init()`, on some layouts and not on others.
- MS-DOS 2 tells a program nothing of where it was found, so gork looks
  for `gork.exe` in the current directory and along `PATH` to find
  `gork.cnf`. It also leaves `INT 2Fh` at 0000:0000, which PDCurses calls
  while it waits for a key: gork points it at the BIOS's `IRET`. And it
  has no commit call, so the TUI closes a duplicate handle instead.
  Compaq's `COMMAND.COM` 2.11 silently runs nothing typed with a
  directory (`..\gork`, `c:\gork`): put gork's directory on `PATH`.

It passes the ten `dostest.py` checks, TUI included, on FreeDOS, MS-DOS
2.11, 3.30 and 6.22, DR-DOS 7.03 and in a DOS box under Windows 1.04, 2.11/386 and 3.1. All
of that on QEMU's 486, which takes 386 instructions without a word.
`testing/dosboxtest.py` runs it on DOSBox-X's 8086 and 286, which do not:
tool calls over the network on the 286, the TUI on the 8086. The 8086's
network is not tried there: the emulated NE2000 loses the byte-wide reads
the Crynwr driver makes on an 8086. No real 8086 or 286 has run it.

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
