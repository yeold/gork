# Progress log

Where the porting and testing work stands, written so another session can
pick it up. Newest state first; update it as each step finishes.

Related files: [TODO-OSES.md](TODO-OSES.md) (systems still to try),
[TODO-DOS.md](TODO-DOS.md) (the DOS port), [BUILD.md](BUILD.md) (how to
build on each system), README "Tested systems" (results table).

## Current state (2026-10-12)

- Released: v0.8-beta (`protocol = openai`, macOS and Cygwin). Naming
  since v0.7-beta: tag
  `vX.Y-beta`, version `X.Y~beta` in the four version files
  (`configure.ac`, `gork.spec`, `debian/changelog`, fallback `VERSION` in
  `main.c`). Everything before v1.0 is published as a pre-release.
- Branches: `dev`, `master` (Forgejo) and `main` (GitHub) have the same
  tree at v0.8-beta. Work happens on `dev`; `master` gets
  the same commits cherry-picked, and `main` one commit with the same tree
  (`git commit-tree 'dev^{tree}' -p github/main`).
- In v0.7-beta since v0.6: a `PATH_MAX` fallback (GNU/Hurd), a larger
  status-line buffer, `configure` falling back to `libncursesw`, DOS
  refusing an `allow` directory that does not exist, test changes
  (`looptest.py` polls for a killed child; `dostest.py` runs on other DOS
  versions and has nine checks), and the results of the overnight run
  below.
- New in v0.8-beta: `protocol = openai` (chat completions: llama.cpp,
  Ollama, OpenAI and the like), marked untested in the README. The conversation stays in
  Messages API form; `to_openai()` and `from_openai()` in `main.c`
  translate the request and the reply inside `post()`. Checked against the
  fake server in `looptest.py` only: no real server has been tried. The
  DOS build has it and passes its nine checks (Jenkins, v0.8-beta), none
  of which uses the protocol.
- Tests: `testing/looptest.py` (17 checks since the OpenAI one; the tables
  below say 16, which it was then; Unix) and `testing/dostest.py`
  (9 checks, DOS in QEMU). Jenkins runs the DOS one on every build of
  `main` and of tags.

## What to do next

1. `protocol = openai` is untested: try it against a real server (Ollama
   or llama.cpp), then take
   "untested" out of the README and `main.c`. macOS and Cygwin are done
   (below).
2. HP-UX: find a compiler for PA-RISC 1.1, or try 11i v2/v3 on QEMU's
   C3700. AIX 7.2 on `qemu-system-ppc64` was not started: the media is
   several GB and an install under emulation takes hours.
3. OpenBSD/sparc64: get the installed kernel to boot, to run the four
   checks that need a pseudo-terminal.
4. Open questions: the Minix console glitch (below) and the DOS `stat()`
   and Esc question (TODO-DOS.md).
5. Not attempted, and why: NeXTSTEP, SunOS 4, UnixWare/OpenServer, QNX,
   BeOS, OS/2 (long manual installs, or a compiler has to be found first);
   Plan 9, UEFI, ZealOS and the like are ports, not builds.

## Overnight QEMU run (started 2026-10-11, late)

Instruction: test every system from TODO-OSES.md that QEMU can run, and
commit and push to `dev` after each result. One row per system, in the
order tried. VM directories are under `~/.cache/gork-vm/`.

| System | Result | How | Notes |
|---|---|---|---|
| Debian GNU/Hurd 2026-03, i386 (gcc 15, glibc 2.42) | passes all 16 checks | `hurd/`, `scripts/hurd.sh`, ssh on port 2230 with `hurd/ssh.sh` | Needed a `PATH_MAX` fallback in `main.c`. The console keyboard does not work in QEMU ("kbd: queue full"), so the ssh key was written into the image offline with `debugfs` (partition 2 starts at sector 1953792). One test assumed a killed child is gone after 0.2 s; it now polls. Slow: a build takes about 10 minutes. |
| DragonFly BSD 6.4.2 (gcc 8.3) | passes all 16 checks | `dfly/`, `scripts/dfly.sh`; the live image boots read-write, so no install. On the console: log in as root, add the key from `dfly/key.pub` to `/root/.ssh/authorized_keys`, `dhclient em0; service sshd onestart`; then `dfly/ssh.sh` (port 2231). Root's shell is csh: pipe scripts to `ssh.sh sh`. | Base ncurses is private; build against the `ncurses` package with `CPPFLAGS="-I/usr/local/include -I/usr/local/include/ncurses" LDFLAGS=-L/usr/local/lib`. `pkg install python311` for the suite. gcc 8 found a status-line buffer that could overflow by a few bytes with very long model and host names; fixed. |
| NetBSD 11.0 amd64 (gcc 12.5, native curses) | passes all 16 checks | `netbsd/`, `scripts/netbsd.sh` (live image, grown to 6 GB with `truncate`; it resizes itself). Console in graphics mode: read it with `scripts/shot netbsd`. On the console: root, key into `/root/.ssh/authorized_keys`, `echo PermitRootLogin prohibit-password >> /etc/ssh/sshd_config; service sshd restart`; then `netbsd/ssh.sh` (port 2232). | Plain `./configure && make`. Python: `PKG_PATH=https://cdn.netbsd.org/pub/pkgsrc/packages/NetBSD/amd64/11.0/All /usr/sbin/pkg_add python313`. The TUI draws correctly on the NetBSD console, so the Minix status-bar glitch is not in NetBSD curses as such. |
| Linux on s390x, armhf, arm64, ppc64el, riscv64, i386 (Debian 13, gcc 14) | all six pass all 16 checks | No VM: a `debian:stable` container with `gcc-<triplet>`, `libncurses-dev:<arch>` and `qemu-user`; `./configure --host=<triplet>`; the suite runs a wrapper script (`qemu-<cpu> -L /usr/<triplet> ./gork "$@"`) as the binary. | User-mode emulation, so the kernel is the host's. s390x is the big-endian one. Not tried: 32-bit big-endian (powerpc, m68k, hppa), which need Debian ports packages. |
| MS-DOS 6.22 (the DOS `gork.exe`) | passes all 8 DOS checks | `GORK_DOS_FLOPPY=~/.cache/gork-vm/dostest/msdos622-1.img python3 testing/dostest.py out/gork-0.6-dos.zip`; the floppy is install disk 1 from archive.org item `disk-1_202101` | `dostest.py` learned to boot a DOS other than FreeDOS. Same binary as FreeDOS. |
| MidnightBSD 4.0.8 amd64 (clang 19.1.7) | passes all 16 checks | `mnbsd/`, `scripts/mnbsd.sh`; installed to `disk.qcow2` with the installer (ZFS, defaults, root password `gork12`); `mnbsd/ssh.sh` (port 2234) | The live CD has no compiler, so it has to be installed. The installer and the first boot ask questions on the VGA console (`scripts/m mnbsd text` reads it); the first boot asks five before sshd starts (answers: us, no, no, no, no). `mport install python311` for the suite. Plain `./configure && make`. |
| DR-DOS 7.03 (the DOS `gork.exe`) | passes 7 of 8 DOS checks; the 8th could not run | `GORK_DOS_ONE_DISK=1 GORK_DOS_FLOPPY=~/.cache/gork-vm/dostest/drdos703-1.img python3 testing/dostest.py out/gork-0.6-dos.zip`; the floppy is the first 1474560 bytes of `DISK01.144` from archive.org item `drdos-703` | DR-DOS goes by the CHS fields of the partition table, which the test wrote wrongly (fixed), and its install floppy never showed the second hard disk, so the D: check is skipped. The test no longer depends on the wording of `ver` or the format of `dir /b`. |
| OpenBSD 7.9 on SPARC64 (QEMU sun4u, gcc 4.2.1) | builds; 12 of 16 checks pass, the 4 TUI checks could not run | `obsd-sparc64/`, `scripts/obsdinst-sparc64.py` (unattended install from `install79.iso`, 5 minutes), `scripts/obsd64.sh`, `obsd-sparc64/ssh.sh` (port 2235) | The installed kernel dies at boot in QEMU 11.1 ("Unhandled Exception 0x30"), the installer's `bsd.rd` does not. So: `up obsd-sparc64 obsd64.sh -prom-env 'auto-boot?=false'`, at the `0 >` prompt `boot disk bsd.rd`, `S` for a shell, then `cd /dev; sh MAKEDEV wd0; cd /; fsck -p /dev/rwd0a /dev/rwd0d /dev/rwd0e; mount /dev/wd0a /mnt; mount /dev/wd0d /mnt/usr; mount /dev/wd0e /mnt/home`, `ifconfig hme0 10.0.2.15 netmask 255.255.255.0 up; route -q add default 10.0.2.2`, `chroot /mnt /usr/bin/ssh-keygen -A; chroot /mnt /usr/sbin/sshd`, and inside `ifconfig lo0 inet 127.0.0.1 up`. That ramdisk kernel has no pseudo-terminals, so `check_trust` and the three TUI checks fail with "out of pty devices"; everything else passes. Fix needed for a full run: a way to boot the real kernel. |
| DOS follow-up (FreeDOS) | all 9 DOS checks pass | `python3 testing/dostest.py` | New check and fix: an `allow` directory that does not exist is refused. Doing that with `stat()` broke Esc-to-abort in the TUI; `opendir()` does not. Why is open: see the last item in TODO-DOS.md. |
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
