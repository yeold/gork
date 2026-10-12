# Progress log

Where the porting and testing work stands, written so another session can
pick it up. Newest state first; update it as each step finishes.

Related files: [TODO-OSES.md](TODO-OSES.md) (systems still to try),
[TODO-DOS.md](TODO-DOS.md) (the DOS port), [BUILD.md](BUILD.md) (how to
build on each system), README "Tested systems" (results table).

## Current state (2026-10-12)

- **No release until the user says so.** The next one is v1.0, a full
  release and not a beta (the user's instruction of 2026-10-12, after
  v0.10-beta). Commits and pushes to the branches go on as before.

- Released: v0.10-beta (version in the TUI's bar and window title;
  v0.9-beta had the native Windows build and the macOS packages,
  v0.8-beta `protocol = openai`, macOS and Cygwin). Naming
  since v0.7-beta: tag
  `vX.Y-beta`, version `X.Y~beta` in the four version files
  (`configure.ac`, `gork.spec`, `debian/changelog`, fallback `VERSION` in
  `main.c`). Everything before v1.0 is published as a pre-release.
- Branches: `dev`, `master` (Forgejo) and `main` (GitHub) have the same
  tree at v0.10-beta. Work happens on `dev`; `master` gets
  the same commits cherry-picked, and `main` one commit with the same tree
  (`git commit-tree 'dev^{tree}' -p github/main`).
- In v0.7-beta since v0.6: a `PATH_MAX` fallback (GNU/Hurd), a larger
  status-line buffer, `configure` falling back to `libncursesw`, DOS
  refusing an `allow` directory that does not exist, test changes
  (`looptest.py` polls for a killed child; `dostest.py` runs on other DOS
  versions and has nine checks), and the results of the overnight run
  below.
- New in v0.8-beta: `protocol = openai` (chat completions: llama.cpp,
  Ollama, OpenAI and the like). The conversation stays in
  Messages API form; `to_openai()` and `from_openai()` in `main.c`
  translate the request and the reply inside `post()`. The user confirmed
  it working with GLM-5.3-Flash, tool calls included (2026-10-12, from
  the Windows build on Windows 11); here it only ever met the fake server
  in `looptest.py`. Other servers (Ollama, llama.cpp, OpenAI) are
  untried. The
  DOS build has it and passes its nine checks (Jenkins, v0.8-beta), none
  of which uses the protocol.
- New in v0.9-beta: macOS packages (`.pkg` and `.tar.gz`, universal,
  unsigned). The `macos` job of the GitHub workflow builds them; on a tag
  it waits for the release Jenkins creates and uploads them there. So they
  are on the GitHub release only, not on Forgejo, and a release needs both
  the Jenkins tag build and the Actions run to finish.
- New in v0.9-beta: a native Windows `gork.exe` (MinGW, PDCurses, Winsock
  1.1; `port.h`, `win.c`, `Makefile.w32`, `winbuild.sh`), one binary meant
  for 9x and NT. Jenkins builds `gork-<version>-win32.zip` and runs
  `testing/wintest.py` under Wine; GitHub Actions cross-builds it and runs
  the same checks on Windows. Works on Windows 98 SE in QEMU (section
  below); NT 4 to XP and Windows 95 are untried. `port.h`
  defines `GORK_NO_FORK` for DOS and Windows, which now share the code
  that used to be `#ifdef __MSDOS__` only.
- New in v0.10-beta: the TUI's top bar and the window title show the
  version; messages on DOS and Windows name the real config path.
- Vulnerability scan: `.github/workflows/codeql.yml` runs CodeQL
  (`security-extended`) on the C, cJSON included, and on the workflows,
  at every push to GitHub `main` and weekly. It sees the Linux build only,
  not the DOS and Windows code. Findings are under the repository's
  Security tab and need a login to read: nobody has looked at them yet.
  The vendored cJSON is 1.7.19, the latest release as of 2026-10-12.
- Tests: `testing/looptest.py` (17 checks since the OpenAI one; the tables
  below say 16, which it was then; Unix) and `testing/dostest.py`
  (9 checks, DOS in QEMU). Jenkins runs the DOS one on every build of
  `main` and of tags.

## What to do next

1. Windows: NT 3.51, both Windows 95s and 98 SE are done by hand
   (below), and the user reported
   `gork.exe` from the v0.9-beta zip working on Windows 11, the TUI too
   (2026-10-12; whether `run_command` was used from it is not known).
   NT 4.0, 2000 and XP are done too. Left: an automated TUI check (Wine draws it through a pseudo-terminal
   one character at a time, which was only read by hand); and the Windows
   98 run itself is not scripted, so Jenkins does not repeat it.
2. macOS and Cygwin are done (below).
3. HP-UX: find a compiler for PA-RISC 1.1, or try 11i v2/v3 on QEMU's
   C3700. AIX 7.2 on `qemu-system-ppc64` was not started: the media is
   several GB and an install under emulation takes hours.
4. OpenBSD/sparc64: get the installed kernel to boot, to run the four
   checks that need a pseudo-terminal.
5. Open questions: the Minix console glitch (below) and the DOS `stat()`
   and Esc question (TODO-DOS.md).
6. Not attempted, and why: NeXTSTEP, SunOS 4, UnixWare/OpenServer, QNX,
   BeOS, OS/2 (long manual installs, or a compiler has to be found first);
   Plan 9, UEFI, ZealOS and the like are ports, not builds.

## Windows NT 3.51, the first Windows 95, and Me in QEMU (2026-10-12)

Results, all with the released v0.10-beta `gork.exe`, unchanged:

- **Windows NT 3.51 Workstation** (`ver`: "Windows NT Version 3.51"):
  works, once `msvcrt.dll` is beside it (NT 3.51 has none; NT 4's copy was
  used). `--version`, the batch round trip of ten tool calls, and the TUI
  with approvals and commands. The file is marked for Windows 4.0 and NT
  3.51 runs it all the same; a 3.10-marked build was made and not needed.
  No `USERPROFILE` there, so `gork.cnf` is read from beside `gork.exe`.
- **The first Windows 95** (`ver`: 4.00.950; boot files dated 1995-07-11):
  works, with `msvcrt.dll` added, exactly as OSR2 did: batch round trip
  and the TUI with approvals and commands.
- **Windows Me**: not tested. Its Setup rejects the product key in the
  archive.org item and both serials on WinWorldPC's page for this very
  ISO ("Windows Me (101 - Retail Full)", the same file from both sites),
  with the clock at 2026 and at 2001. Not pursued further.

How, for a repeat, in `/vm/nt351`, `/vm/95a` and `/vm/me` of the same
volume:

- NT 3.51: archive.org item `ntwks351_iso` (SHA1 `4d3a8964...`), no
  product key asked. `prepnt351.py`, `runnt351.sh`: as NT 4, from DOS
  with `winnt /b /u`. **`CPU=486`**: on `-cpu pentium` text-mode setup
  dies with STOP 0x50. 32 MB, `-vga std`, ISA NE2000 at 0x300 IRQ 3. The
  graphical part asks: name, computer name, the card (Enter; then Left,
  Enter for "use them anyway"), and reports that the network could not
  start (choose No): only the Workstation service fails, the card's
  driver is fine. Log on: Ctrl-Alt-Del, the user name is remembered, so
  just Enter. TCP/IP is not installed: `control ncpa.cpl`, Alt-S (Add
  Software), `t` for "TCP/IP Protocol and related components", Alt-E for
  DHCP, the path `d:\i386`, OK, restart. Commands through File, Run
  (Alt-F, R). `floppy351/T351.BAT`.
- The first Windows 95: archive.org item `win95_202004` (`Win95.iso`),
  its key in the item's description. `prep9x.py 95a WIN95 KEY`, then
  `CPU=pentium MEM=64 VGA=std NIC=ne2k_isa,netdev=n0,iobase=0x200,irq=3
  run9x.sh 95a`, with `ICOUNT=3` for every boot from the disk. As OSR2,
  except: the product number goes into one box, dashes and "OEM"
  included; Setup asks for a startup disk (Tab, Enter cancels); and the
  network card's property sheet has two tabs, so one Ctrl-Tab reaches
  Resources (Alt-S, four Tabs, Enter).
- `prep9x.py DIR SRC KEY` and `run9x.sh DIR` are the general forms of
  the 98 and 95 scripts.
- WinWorldPC: a product page lists `/download/<id>` pages, each with
  `/download/<id>/from/<mirror>` links that redirect to a `.7z` on
  `dl.winworldpc.com` (the `dl-alt1` mirror did not answer). Serials are
  on the product page.

## Windows 2000 and XP in QEMU (2026-10-12)

Result: the released v0.10-beta `gork.exe` works on Windows 2000
Professional (`ver`: 5.00.2195) and Windows XP Professional SP3
(5.1.2600), unchanged: `--version`, the batch round trip of ten tool
calls, and the TUI with a reply, approvals, commands run from it and the
window title.

How, for a repeat (about 45 minutes for 2000, 55 for XP, nearly all
unattended). In `/vm/2k` and `/vm/xp` of the same volume; scripts
`prepnt5.py DIR NAME KEY` and `runnt5.sh DIR`.

- Media: archive.org items `win2kproiso` (SHA1 `d0b0ad04...`, key in its
  `Product Key.txt`) and `WinXPProSP3x86` (SHA1 `66ac289a...`, a volume
  licence CD, key on the item's page).
- Both CDs boot and install by themselves with an answer file `WINNT.SIF`
  on a floppy (`sif.img`): partitioning, key, network, automatic logon as
  Administrator. Nothing to press. Disk: an empty 4 GB qcow2. The CD stays
  in; once the disk has a system the CD's "press any key" times out.
- Windows 2000 needs `runnt5.sh 2k -win2k-hack`, `CPU=qemu32` and ACPI
  left on. With `-machine acpi=off` and `-cpu pentium3` its graphical
  setup reset the machine at "Installing components", again and again.
- XP: `CPU=qemu32 runnt5.sh xp`. Network card `rtl8139` for both; both
  have the driver and get an address by DHCP.
- Test floppy as for NT 4 (`floppy/TNT.BAT`), run with `cmd /k a:\tnt.bat`
  from Start, Run. `shutdown -s -t 0` turns XP off.

## Windows NT 4.0 in QEMU (2026-10-12)

Result: the released v0.10-beta `gork.exe` works on Windows NT
Workstation 4.0 as shipped in 1996 (`ver`: "Windows NT Version 4.0", no
service pack), with NT's own `msvcrt.dll`: `--version`, the batch round
trip (ten tool calls, `run_command` through `cmd.exe`), the config found
in `%USERPROFILE%` (`C:\WINNT\Profiles\Administrator\gork.cnf`), and
the TUI with a reply, the approval prompts, commands run from it and the
window title. Nothing had to be changed in gork.

How, for a repeat (about 25 minutes, the easiest of the three). Same
container and volume, in `/vm/nt4`; scripts `prepnt.py`, `runnt.sh`,
`stopnt.sh`.

- Media: archive.org item `WindowsNTWorkstation4_201708` (ISO SHA1
  `7061f78c...`, volume `NTWKS40A`), with `Key.txt` beside it.
- Setup runs from DOS: the Windows 98 startup floppy, `fdisk /mbr`,
  `smartdrv`, `echo y| lock c:` (DOS 7 refuses WINNT.EXE's write of the
  boot sector otherwise), then `winnt /b /s:d:\i386 /u:d:\unattend.txt`
  with `I386` on the second FAT disk. `UNATTEND.TXT` is honoured in full:
  after the DOS phase (ten minutes) start QEMU from the disk and it runs
  to the logon screen by itself. Log on with `sendkey ctrl-alt-delete`,
  Administrator, no password.
- `-cpu pentium`, full speed (no `-icount`), `-vga cirrus`.
- Network: with QEMU's `pcnet`, NT 4's bundled AMD driver stops Setup with
  "The network failed to start (0x42a)". An ISA NE2000 at 0x300 is
  detected and works, on **IRQ 3**: on IRQ 5 the driver reads the card's
  address but DHCP gets nothing.
- `floppy/TNT.BAT` is the batch run; it copies `gork.cnf` to
  `%USERPROFILE%`. `c-installed.img` is the finished disk.

## Windows 95 OSR2 in QEMU (2026-10-12)

Result: the released v0.10-beta `gork.exe` works on Windows 95 OSR2
(`ver`: 4.00.1111), once `msvcrt.dll` is beside it: `--version`, the same
batch round trip as on 98 (ten tool calls, `run_command` through
`COMMAND.COM`), and the TUI with a reply, the version in its bar and in
the window title. Without `msvcrt.dll`: "A required .DLL file,
MSVCRT.DLL, was not found" (the copy used came from the Windows 98 SE
install). No other import is missing. The first Windows 95 (4.00.950)
was not tried: its ISO on archive.org comes without a key.

How, for a repeat. Same container, volume and tools as for 98, in
`/vm/95`; scripts `prep95.py`, `run95.sh`, `stop95.sh` beside the others.

- Media: archive.org item `win-95-osr-2` (SHA1 `9df2bed9...`), key on the
  item's page. The CD does not boot; the Windows 98 startup floppy from
  the 98 run starts Setup. **No `sys c:` from that floppy**: Setup keeps
  the IO.SYS it finds, and the result boots as "Windows 98".
- `MSBATCH.INF` is mostly ignored. By hand: Alt-Y for the licence; at the
  key screen Enter on "Re-enter", then all 17 digits in one go (the three
  boxes act as one field); the network adapter: Alt-M, `n` four times,
  Down (Novell/Anthem), Alt-D, Down (NE2000 Compatible), Enter.
- After the file copy Setup reboots: start QEMU from the disk then
  (`run95.sh -boot order=c`), or `-boot once=a` runs Setup again.
- "While initializing device IOS: Windows protection error" at full
  speed: Windows 95's fault on fast CPUs. `ICOUNT=3 run95.sh` (QEMU
  `-icount shift=3`, 125 million instructions a second) gets past it and
  is needed on every boot. Everything is slow then: minutes per boot.
- Network: an ISA NE2000 (`ne2k_isa`), no PCI needed. Windows sets the
  driver to I/O 0x200 and IRQ 3, so the emulated card is put there; then
  in Device Manager the card shows "Code 27" until its Resources tab gets
  "Set Configuration Manually", OK, and a shutdown. TCP/IP was installed
  by Setup here (`MSBATCH.INF` did that much).
- `floppy/T95.BAT` and `T95B.BAT` are the batch runs; `c-installed.img`
  is the finished disk.

## Windows 98 SE in QEMU (2026-10-12)

Result: the MinGW `gork.exe` works on Windows 98 Second Edition
(4.10.2222), on QEMU's `pentium` and `pentium2` CPUs. Checked by hand
against a fake API on the host (10.0.2.2): `--version`; a batch round trip
with `read_file` (either slash, either case), `write_file`, `edit_file`,
a path outside the working directory refused, `list_dir`, and
`run_command` (`echo`, `ver`, `dir /b`) with output and exit status; the
TUI with a reply, the approval prompts, a command run from it, `/quit`.
On `pentium` the TUI was only taken as far as one reply.

What 98 found, all fixed:

- `COMMAND.COM` cannot copy to a name starting with a dot, so Windows now
  uses the DOS names `gork.cnf` and `_gork`.
- Winsock's `gethostbyname` does not take a numeric address there
  ("cannot resolve 10.0.2.2"): `relay.c` tries `inet_addr` first.
- The TUI drew nothing: PDCurses draws into a second screen buffer, which
  9x never shows. `tui.c` sets `PDC_RESTORE_SCREEN=0` and gives curses its
  own `CONOUT$` handle (`win_own_console`), since fd 1 is redirected.
- On the `pentium` CPU: "illegal operation". gcc's default is the Pentium
  Pro, and so is Debian's build of MinGW's own `strtod` and `printf`.
  Now `-march=i486`, `-D__USE_MINGW_ANSI_STDIO=0`, and a `strtod` in
  `win.c` that goes through msvcrt's `sscanf`. Two `cmov` remain, in
  start-up code (`_mark_section_writable`, `__GetPEImageBase`); the
  Pentium run did not reach them.

How it was set up, for a repeat (about 90 minutes, no KVM). Scripts are in
`~/.cache/gork-vm/win98/` on this machine (`lab`); the VM itself is in the
podman volume `w98vm`, mounted at `/vm` in a `debian:stable` container
named `w98` with `qemu-system-x86 mtools p7zip-full cabextract netpbm`.

- Media: archive.org item `windows-98-se-isofile`, saved as `/vm/w98.iso`
  (SHA1 `fa040cd3...`); the product key is on the item's page.
- `prep.py KEY` builds `boot.img` (the CD's startup floppy with our
  `autoexec.bat`: `sys c:`, `fdisk /mbr`, `setup /is /iq /im /ie
  d:\msbatch.inf`), an empty `c.img` and a `d.img` holding `WIN98\` and
  `MSBATCH.INF`, so no CD driver is needed. Three things it has to get
  right: `JO.SYS` removed from the floppy (else it boots the hard disk);
  partitions on whole cylinders of the 16 x 63 geometry (else `mformat` writes another geometry and
  Setup stops with SU0013); disks under 504 MB.
- `run.sh` starts QEMU (`-M pc -cpu pentium2 -m 128 -vga cirrus`,
  `ne2k_pci`, `-no-reboot` in a loop): after a reboot inside one QEMU
  process the guest gets no interrupts any more, so each guest reboot
  needs a new process. First start: `run.sh -boot once=a,order=c`.
  `CPU=pentium FLOPPY=g.img run.sh -boot order=c` for the other CPU.
- `m.py` drives the monitor: `key`, `type`, `shot` (PNG), `text`.
- Setup stops four times in spite of `MSBATCH.INF`: user name (Enter),
  licence (Alt-A, Enter), product key (Enter), Finish (Enter).
- The first boot from disk hangs on the logo: add `Logo=0` to `[Options]`
  in `C:\MSDOS.SYS` (from Linux with `mattrib` and `mcopy`).
- No network card at first: "Plug and Play BIOS" fails in Device Manager.
  Update its driver, choosing "PCI bus" from the list of all hardware;
  after the restart Windows finds the PCI devices, the Realtek RTL8029
  among them, and each wizard is Enter all the way.
- Files go in on a floppy image (`change floppy0 /vm/g.img`); commands
  through Start, Run (`sendkey ctrl-esc`, `r`). `fake98.py` is the fake
  API; `floppy/T3.BAT` the batch run. `c-installed.img` is a copy of the
  finished disk.

## Overnight QEMU run (started 2026-10-11, late)

Instruction: test every system from TODO-OSES.md that QEMU can run, and
commit and push to `dev` after each result. One row per system, in the
order tried. VM directories are under `~/.cache/gork-vm/`.

| System | Result | How | Notes |
|---|---|---|---|
| Debian GNU/Hurd 2026-03, i386 (gcc 15, glibc 2.42) | passes all 16 checks | `hurd/`, `scripts/hurd.sh`, ssh on port 2230 with `hurd/ssh.sh` | Needed a `PATH_MAX` fallback in `main.c`. The console keyboard does not work in QEMU ("kbd: queue full"), so the ssh key was written into the image offline with `debugfs` (partition 2 starts at sector 1953792). One test assumed a killed child is gone after 0.2 s; it now polls. Slow: a build takes about 10 minutes. |
| DragonFly BSD 6.4.2 (gcc 8.3) | passes all 16 checks | `dfly/`, `scripts/dfly.sh`; the live image boots read-write, so no install. On the console: log in as root, add the key from `dfly/key.pub` to `/root/.ssh/authorized_keys`, `dhclient em0; service sshd onestart`; then `dfly/ssh.sh` (port 2231). Root's shell is csh: pipe scripts to `ssh.sh sh`. | Base ncurses is private; build against the `ncurses` package with `CPPFLAGS="-I/usr/local/include -I/usr/local/include/ncurses" LDFLAGS=-L/usr/local/lib`. `pkg install python311` for the suite. gcc 8 found a status-line buffer that could overflow by a few bytes with very long model and host names; fixed. |
| NetBSD 11.0 amd64 (gcc 12.5, native curses) | passes all 16 checks | `netbsd/`, `scripts/netbsd.sh` (live image, grown to 6 GB with `truncate`; it resizes itself). Console in graphics mode: read it with `scripts/shot netbsd`. On the console: root, key into `/root/.ssh/authorized_keys`, `echo PermitRootLogin prohibit-password >> /etc/ssh/sshd_config; service sshd restart`; then `netbsd/ssh.sh` (port 2232). | Plain `./configure && make`. Python: `PKG_PATH=https://cdn.netbsd.org/pub/pkgsrc/packages/NetBSD/amd64/11.0/All /usr/sbin/pkg_add python313`. The TUI draws correctly on the NetBSD console, so the Minix status-bar glitch is not in NetBSD curses as such. |
| Linux on s390x, armhf, arm64, ppc64el, riscv64, i386 (Debian 13, gcc 14) | all six pass all 16 checks | No VM: a `debian:stable` container with `gcc-<triplet>`, `libncurses-dev:<arch>` and `qemu-user`; `./configure --host=<triplet>`; the suite runs a wrapper script (`qemu-<cpu> -L /usr/<triplet> ./gork "$@"`) as the binary. | User-mode emulation, so the kernel is the host's. s390x is the big-endian one. 32-bit big-endian (powerpc, m68k, hppa) came later: see that row. |
| MS-DOS 6.22 (the DOS `gork.exe`) | passes all 8 DOS checks | `GORK_DOS_FLOPPY=~/.cache/gork-vm/dostest/msdos622-1.img python3 testing/dostest.py out/gork-0.6-dos.zip`; the floppy is install disk 1 from archive.org item `disk-1_202101` | `dostest.py` learned to boot a DOS other than FreeDOS. Same binary as FreeDOS. |
| MidnightBSD 4.0.8 amd64 (clang 19.1.7) | passes all 16 checks | `mnbsd/`, `scripts/mnbsd.sh`; installed to `disk.qcow2` with the installer (ZFS, defaults, root password `gork12`); `mnbsd/ssh.sh` (port 2234) | The live CD has no compiler, so it has to be installed. The installer and the first boot ask questions on the VGA console (`scripts/m mnbsd text` reads it); the first boot asks five before sshd starts (answers: us, no, no, no, no). `mport install python311` for the suite. Plain `./configure && make`. |
| DR-DOS 7.03 (the DOS `gork.exe`) | passes 7 of 8 DOS checks; the 8th could not run | `GORK_DOS_ONE_DISK=1 GORK_DOS_FLOPPY=~/.cache/gork-vm/dostest/drdos703-1.img python3 testing/dostest.py out/gork-0.6-dos.zip`; the floppy is the first 1474560 bytes of `DISK01.144` from archive.org item `drdos-703` | DR-DOS goes by the CHS fields of the partition table, which the test wrote wrongly (fixed), and its install floppy never showed the second hard disk, so the D: check is skipped. The test no longer depends on the wording of `ver` or the format of `dir /b`. |
| OpenBSD 7.9 on SPARC64 (QEMU sun4u, gcc 4.2.1) | builds; 12 of 16 checks pass, the 4 TUI checks could not run | `obsd-sparc64/`, `scripts/obsdinst-sparc64.py` (unattended install from `install79.iso`, 5 minutes), `scripts/obsd64.sh`, `obsd-sparc64/ssh.sh` (port 2235) | The installed kernel dies at boot in QEMU 11.1 ("Unhandled Exception 0x30"), the installer's `bsd.rd` does not. So: `up obsd-sparc64 obsd64.sh -prom-env 'auto-boot?=false'`, at the `0 >` prompt `boot disk bsd.rd`, `S` for a shell, then `cd /dev; sh MAKEDEV wd0; cd /; fsck -p /dev/rwd0a /dev/rwd0d /dev/rwd0e; mount /dev/wd0a /mnt; mount /dev/wd0d /mnt/usr; mount /dev/wd0e /mnt/home`, `ifconfig hme0 10.0.2.15 netmask 255.255.255.0 up; route -q add default 10.0.2.2`, `chroot /mnt /usr/bin/ssh-keygen -A; chroot /mnt /usr/sbin/sshd`, and inside `ifconfig lo0 inet 127.0.0.1 up`. That ramdisk kernel has no pseudo-terminals, so `check_trust` and the three TUI checks fail with "out of pty devices"; everything else passes. Fix needed for a full run: a way to boot the real kernel. |
| DOS follow-up (FreeDOS) | all 9 DOS checks pass | `python3 testing/dostest.py` | New check and fix: an `allow` directory that does not exist is refused. Doing that with `stat()` broke Esc-to-abort in the TUI; `opendir()` does not. Why is open: see the last item in TODO-DOS.md. |
| Linux on m68k, powerpc, hppa (Debian sid, gcc 16.2; 2026-10-12) | all three pass all 17 checks | A `debian:sid` container run with `--init`: `gcc-<triplet>`, `libc6-dev-<arch>-cross`, `qemu-user`; `libncurses-dev:<arch>` only downloaded from debian-ports (`apt-get download` after adding the architecture and a `debian-ports` source) and unpacked with `dpkg-deb -x`; `./configure --host=<triplet> CPPFLAGS=-I<root>/usr/include LDFLAGS="-L<root>/usr/lib/<triplet> -static" LIBS=-ltinfo`; the suite runs a wrapper (`qemu-m68k`, `qemu-ppc`, `qemu-hppa`). | The 32-bit big-endian ones. Static, as the ports' libraries cannot be installed beside amd64's. Without `--init` the last check fails ("background child survived the timeout") for a native build too: nothing reaps the orphan. hppa is PA-RISC 1.1, the processor HP-UX had no compiler for. |
| HP-UX 11.11 (PA-RISC, B160L) | no result: no usable compiler | `hpux/`, `scripts/hpux.sh`; see the section below | OS installs and runs; gork was never compiled. |
| Linux with uClibc (static x86-64, gcc 15.3) | passes all 16 checks | A `debian:stable` container: Bootlin toolchain `x86-64--uclibc--stable-2026.08-1`, ncurses 6.5 cross-built into a prefix (`--without-shared --without-progs`, needs a native gcc too), then `./configure --host=x86_64-buildroot-linux-uclibc CPPFLAGS=-I.. LDFLAGS="-L.. -static"`. | That ncurses build only makes `libncursesw`, which `configure` did not look for; it does now, as a last resort. |

### HP-UX 11i v1 (11.11) on PA-RISC: installed, but no compiler

Result: HP-UX 11.11 installs and runs in QEMU, but gork could not be built
there, because no usable C compiler could be found for that machine. Not a
gork failure; nothing was compiled.

- The bundled `cc` is K&R only.
- The gcc 4.2.3 depot from the HP-UX porting centre installs, but its
  binaries are PA-RISC 2.0 and the emulated B160L has a PA-RISC 1.1
  processor: `sh: gcc: Execute permission denied`. The porting centre has
  no older gcc any more and the Wayback Machine has none of its depots.
- QEMU's PA-RISC 2.0 machine (`-M C3700`) does not help: the installed
  32-bit kernel panics there ("Unidentified cpu type"), and the 64-bit
  install kernel from the same CDs panics with a data page fault. `-M
  B160L -cpu pa-8500` is rejected by QEMU.
- Ways forward: a gcc built for PA-RISC 1.1 (HP-UX 11.00-era depots, if a
  copy turns up), HP's ANSI C compiler from the Application CDs (needs a
  licence codeword), HP-UX 11i v2 or v3 on the C3700, or a cross compiler
  using headers and libraries copied from the guest.

What exists, for a repeat (about 80 minutes):

- Media: archive.org item
  `hp-ux-11i-version-1-mission-critical-operating-environment-sept-2005`,
  four CDs, saved as `~/.cache/gork-vm/hpux/cd1.iso` .. `cd4.iso` (CD 3 and
  4 checked against the item's SHA256 list). All four are needed even for
  the Base OS environment.
- Installed disk: `hpux/disk.img`: HP-UX 11.11
  32-bit, root without password, host name `hpux11`, with the gcc depots
  installed in `/usr/local` and the gork source unpacked in `/gk`.
- Machine: `scripts/hpux.sh` (`qemu-system-hppa -m 512 -nographic`, B160L,
  SCSI disk at ID 6, CD at ID 2, tulip network card). `-nographic` matters:
  without it the firmware uses a graphics console and the serial port stays
  silent. Start with `scripts/up hpux hpux.sh`, then `scripts/serlog hpux`.
- Console: serial. `scripts/hk hpux 'keys' SECS` sends keys and shows the
  new output, `scripts/sv hpux` renders the log tail, `scripts/sc hpux
  'command'` runs a shell command once logged in, `scripts/nx` tabs to
  "Next >" in the install wizard and presses Enter (`NXL=Finish scripts/nx`
  at the end).
- Install: `scripts/up hpux hpux.sh -boot d`, terminal type `2` (VT100),
  Enter on "Install HP-UX", Enter on OK (media only, guided), then Next
  through every wizard step with the defaults (the only environment offered
  is "HP-UX 11i Base OS-32bit"), then Finish. When it asks for the next CD:
  `scripts/m hpux cmd "change scsi0-cd2 /path/cdN.iso"`, then Enter. On
  first boot answer: no (geocustoms), n (network), y, a host name, a time
  zone, y (clock), n (root password), Enter twice.
- Files into the guest: an ISO made with `mkisofs -r` in the CD drive,
  `mount -F cdfs -o ro,cdcase /dev/dsk/c0t2d0 /cdrom`. Depots install with
  `swinstall -x enforce_dependencies=false -s /cdrom/gcc.dep \*`.

## TODO-OSES.md, step 1

| System | Status | Notes |
|---|---|---|
| Linux with musl | done, passes | Alpine 3.24, gcc 15: `./configure && make`, no warnings, all 16 checks. Run in the `alpine:latest` container from the tarball. |
| Minix 3.3.0 | done, works with one glitch | See below. |
| macOS | done, passes | macOS 26.6.2 on arm64, Apple clang 21, the system's ncurses: all 16 checks. `.github/workflows/build.yml`, job `macos`. One test compared a path under `/var` with the resolved `/private/var`; it now resolves it first. The TUI was not looked at by hand. |
| Cygwin | done, passes | Cygwin 3.6.11 on Windows Server 2025, gcc 14.4, ncurses 6.5: all 16 checks, no source change. Same file, job `cygwin`. Needs LF line ends in the checkout. |

The workflow runs on pushes to GitHub `main` and to `ci/*` branches. To
try a tree without touching `main`: `git push github <commit>:refs/heads/ci/try`.
The job logs need a GitHub login, so a failing test's output is also
written as an annotation, which the public API shows:
`https://api.github.com/repos/yeold/gork/commits/<sha>/check-runs`, then
each run's `url` + `/annotations`.

Next after these, per TODO-OSES.md: AIX and HP-UX in QEMU. Another session
added UEFI and U-Boot rows to TODO-OSES.md (not tried, just recorded).

### Minix 3.3.0 (done)

Result: `./configure CC=clang && make` builds with one harmless warning
(clang ignoring `-g`); `gork --version`, a batch round trip and a TUI round
trip (trust prompt, approval, reply, `/quit`) work against `scripts/fake.py`.

Open problem: on the Minix console (`TERM=minix`, 80x25) the whole screen
scrolls up one line when the first reply is drawn, so the status bar is lost
and the input line is off by one. The first frame is correct and an empty
Enter does not trigger it. The same curses on NetBSD 11.0's console draws
correctly. Tried on Minix, all without effect: `idlok(stdscr, FALSE)`;
`stty columns 79`; a terminal description without insert/delete-line and
reverse-index (`infocmp minix`, edited, `tic -o`, `TERMINFO=`); a status bar
that stops one column short of the right edge. Untested: a login over the
network instead of the console, and tracing what gork writes with
`script(1)` to find the byte sequence that scrolls.

How it was set up, for a repeat:

- ISO: `http://download.minix3.org/iso/minix_R3.3.0-588a35b.iso.bz2`.
  The VM directory `~/.cache/gork-vm/minix` holds the ISO and the installed
  4 GB disk (`disk.qcow2`) with clang and a built gork in `/usr/w`.
- Install: `scripts/up minix minix.sh -boot d`, log in as root, `setup`,
  all defaults, `yes` at the confirmation, network card 7 (Intel PRO/1000).
  Boot the result with `scripts/up minix minix.sh -boot c`.
- Network: DHCP does not get an address in QEMU. After each boot:
  `ifconfig -h 10.0.2.15 -n 255.255.255.0; add_route -g 10.0.2.2;
  echo nameserver 10.0.2.3 > /etc/resolv.conf`.
- Compiler: `pkgin update` hung (the 128 MB root file system was full).
  What worked: download `clang-3.4nb1.tgz` and `binutils-2.23.2.tgz` from
  `http://www.minix3.org/pkgsrc/packages/3.3.0/i386/All/` on the host,
  serve them with `python3 -m http.server`, and in the guest, under
  `/usr/w`: `ftp -o c.tgz http://10.0.2.2:PORT/clang-3.4nb1.tgz` and
  `TMPDIR=/usr/w PKG_TMPDIR=/usr/w pkg_add -f c.tgz`. The missing
  dependencies (python, libxml2) do not matter for compiling C.
- Reading the screen: the console scrolls in hardware, so `m minix text`
  shows stale lines. Use `scripts/shot minix [ops]` (saves a PNG) or
  `m minix textall`. Opening `/dev/tty00` blocks, so there is no serial
  shell. A second console is on Alt-F2 (`m minix key alt-f2`).

## How the test machines are driven

Nothing below is in the repository; it lives in `~/.cache/gork-vm` on the
development machine and is rough.

- `scripts/up DIR SCRIPT [qemu args]` starts a VM in the background with
  its monitor on `DIR/mon.sock` and serial port on `DIR/ser.sock`.
- `scripts/m DIR ...` talks to the monitor: `type 'text\n'`, `key f2`,
  `text` (print the 80x25 screen), `until PATTERN SECS`, `wait SECS`.
  `scripts/vt DIR ...` does the same and prints the screen.
- `scripts/ser.py`, `serd.py`, `sk`, `sc` drive a serial console.
- `scripts/fake.py` (port 8099) and `scripts/fake2.py PORT DELAY TOOL JSON`
  are fake Messages APIs for guests; the host is 10.0.2.2 from inside QEMU.
- One launcher per system: `dosvm.sh`, `minix.sh`, `nb10.sh`, `ob20.sh`,
  `fb20.sh`, `fb205.sh`, `sol10.sh`, `sol26.sh`, `sol251.sh`, `osol.sh`,
  `omnivm.sh`, `bsdvm.sh`, `slackvm.sh`. The images they used were deleted
  to save space; the scripts say how each was built.
- `scripts/jk PATH` calls the Jenkins API at
  `http://osgiliath.selea.internal:8080` with the credentials in `~/.netrc`.
  To build a new tag, trigger a scan (`jk '/job/gork/build?delay=0' -X
  POST`), never a manual tag build: a second run fails on existing assets.

## Finished earlier (details in the README table and BUILD.md)

- Oldest Linux: Slackware 2.0.0 (1994). Oldest BSDs: NetBSD 1.0, FreeBSD
  2.0, OpenBSD 2.0. Solaris 2.5.1, 2.6, 10; OpenSolaris 134; OmniOS.
  Solaris 2.4 could not be booted in QEMU.
- DOS port: working, experimental; only real hardware is untested.
