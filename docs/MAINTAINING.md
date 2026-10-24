# Maintaining gork

For whoever cuts releases and keeps the builds running.
[HACKING.md](HACKING.md) is about the source, [BUILD.md](BUILD.md) about
building on each system.

## Branches and versions

- Work happens on `dev`. `master` (Forgejo) and `main` (GitHub) are
  the same history: push `dev` to both (`git push origin dev:master`,
  `git push github dev:main`), so they only ever fast-forward. Until
  2026-10-19 the three were copied commit by commit; the old Forgejo
  history is kept under the tags `old-dev` and `old-master`.
- The version is in four places, changed together: `configure.ac`,
  `gork.spec`, the top of `debian/changelog`, and the fallback `VERSION`
  in `main.c`. A beta is `X.Y~beta` there and `vX.Y-beta` as a tag.
- A `v0.*` tag is published as a pre-release, anything else as a release.

## What a release carries

| Package | Built by | Checked by |
|---|---|---|
| `gork-<v>.tar.gz` (source) | Jenkins, `make dist` | builds the RPM and the deb |
| `.rpm`, `.src.rpm` (x86-64) | Jenkins, Fedora | |
| `.deb` (amd64) | Jenkins, Debian stable | |
| `gork-<v>-linux-<arch>.tar.gz`, ten CPUs, static | Jenkins, `linuxbuild.sh` | `testing/linuxtest.sh`: the suite under qemu-user |
| `gork-<v>-{freebsd,netbsd,openbsd}-{amd64,arm64}.tar.gz`, static | Jenkins, `bsdbuild.sh` (clang against each release's base sets) | `testing/bsdtest.py`: `vmcheck.sh` on the release's own image in QEMU |
| `gork-<v>-uefi.zip` (`gorkx64.efi`, `gorkaa64.efi`) | Jenkins, `uefibuild.sh` (clang) | `testing/uefitest.py`: the UEFI shell under OVMF and AAVMF, and on gork's own TCP/IP under OVMF and U-Boot, in QEMU |
| `gork-<v>-dos.zip` (386, DJGPP) | Jenkins, `dosbuild.sh` | `testing/dostest.py` on FreeDOS in QEMU |
| `gork-<v>-dos16.zip` (8086, Open Watcom) | Jenkins, `dos16build.sh` | the same, and `testing/dosboxtest.py` on an 8086 and a 286 in DOSBox-X |
| `gork-<v>-win32-legacy.zip` (`gork.exe`, `gorknt3.exe`) | Jenkins, `winbuild.sh` | `testing/wintest.py` under Wine; again on Windows by GitHub Actions |
| `gork-<v>-win32.zip` (Cygwin) | GitHub Actions, `cygpack.sh` | `testing/cygpacktest.py` |
| `gork-<v>-macos.pkg`, `-macos.tar.gz` (universal, unsigned) | GitHub Actions | `testing/looptest.py` |

The Linux binaries are static glibc ones. They resolve host names without
the system's libc all the same (tried on Alpine, which has none).

## Making a release

1. Set the version in the four places, with a `debian/changelog` entry:
   its text becomes the release notes.
2. Push `dev` to `master` and `main`, and wait for both builds to pass.
3. Tag `vX.Y` on GitHub's `main`. Jenkins builds the tag, creates the
   release on GitHub and on Forgejo and uploads its packages
   (`release.sh`); the debs, RPMs and tarballs also go to Forgejo's
   package registry. GitHub Actions builds the same tag, waits for the
   release to exist and adds the macOS and Cygwin packages, so those two
   are on GitHub only.
4. If the Cygwin job of the tag run fails, the release lacks
   `gork-<v>-win32.zip`. Fix the job on `main`: its next build adds the
   package to the release of the version in the tree, when that release
   has none. (1.0 got its package that way.)
5. An asset that is already on a release makes its upload fail. To
   replace one, delete the release on that forge and run the tag build
   again; never start a second build of a tag otherwise.

## Tests

| Suite | What it runs |
|---|---|
| `python3 testing/looptest.py ./gork` | The Unix build against fake APIs: 23 checks; those of the TUI or MCP are skipped when the build has none |
| `python3 testing/wintest.py [wine] gork.exe` | The legacy Windows programs: batch mode, and under Wine the TUI with `/save` and `/load` |
| `python3 testing/dostest.py ZIP` | Either DOS zip on FreeDOS in QEMU: ten checks, the TUI among them |
| `python3 testing/dosboxtest.py ZIP` | The 16-bit zip in DOSBox-X: two tool calls over the network on a 286, the TUI on an 8086 |
| `sh testing/linuxtest.sh OUT` | Every Linux binary, under qemu-user |
| `sh testing/vmcheck.sh GORK FAKEAPI [MCPSH]` | Batch checks with only a Bourne shell: on any system, inside a VM |
| `python3 testing/uefitest.py ZIP FAKEAPI [MACHINE]` | gork from the UEFI shell in QEMU: under OVMF (`x64`), AAVMF (`aa64`), OVMF without its network stack (`x64-own`) or U-Boot (`uboot`); twelve checks |
| `python3 testing/bsdtest.py TARGET PACKAGE FAKEAPI` | A BSD package on its own system in QEMU, through `vmcheck.sh`; OpenBSD is installed once, unattended |
| `python testing\cygpacktest.py DIR` | The Cygwin package, without Cygwin; the TUI in a console, with pywinpty |
| `python3 testing/winspxtest.py ZIP` | SPX from Windows: `gork.exe` on a Windows 95 to Me disk image in QEMU, through gork-relay. By hand: it needs an installed Windows |

With `GORK_RELAY` naming a gork-relay binary (`sh testing/getrelay.sh`
builds one), `looptest.py` and `wintest.py` put it in front of every fake
API, as Jenkins does.

`dostest.py` takes other systems through environment variables, described
at its top: another DOS (`GORK_DOS_FLOPPY`, with `GORK_DOS_SMALL_DISKS`
for MS-DOS 3.30, 4.01 and DR-DOS 7.03 and `GORK_DOS_FAT12` for MS-DOS 2), a Windows 1 to 3.1 directory to run the DOS box
of (`GORK_DOS_WINDOWS`; `GORK_DOS_WINDOWS_STANDARD` for standard mode), HIMEM and EMM386 to load (`GORK_DOS_EMM386`),
and a gork-relay binary for the SPX check
(`GORK_RELAY`; with `GORK_DOS_ODI`, Novell's ODI stack instead of PDIPX,
or with `GORK_DOS_IPXCOM`, an `IPX.COM` that WSGEN made),
and an address by DHCP (`GORK_DOS_DHCP`). Jenkins runs it on FreeDOS only, with a gork-relay built by `testing/getrelay.sh`, and the 16-bit zip with DHCP; the others need media
that cannot be put in a repository. Before a release, the 32-bit zip was
last run on MS-DOS 3.30, 4.01 and 6.22, DR-DOS 7.03 and under Windows 1.04, 2.03, 2.11,
3.0 and 3.1, and the 16-bit one on MS-DOS 2.11 (Compaq's, from WinWorldPC), 3.30 and
6.22, DR-DOS 7.03 and under Windows
1.04, 2.11/386 and 3.1.

The old Windows and Unix systems in the README's table were tried by hand
in QEMU; [TODO-REAL.md](TODO-REAL.md) lists what that leaves unproven.

## Code scanning

`.github/workflows/codeql.yml` runs CodeQL (`security-extended`) on the C
as built for Linux, the vendored cJSON included, and on the workflows.
The findings are under the repository's Security tab; the job also prints
them as an annotation, which needs no login.

Eight findings are how gork is meant to work, and are dismissed there:

| Finding | Where | Why it stands |
|---|---|---|
| `cpp/path-injection` (4) | the config file, `~/.gork/trusted`, `~/.gork/mcp.json` | The paths come from `HOME` and `GORK_CONFIG`: the user's own environment naming the user's own files. gork runs with that user's rights and is not setuid |
| `cpp/system-data-exposure` (2) | `write()` in `relay.c` | The request, API key included, is sent to the relay. That is the design, and the README says what it means |
| `cpp/user-controlled-bypass` | the check for an empty key | It only decides whether to print "no API key" before trying; nothing is guarded by it |
| `cpp/world-writable-file-creation` | `save_file()` | A file the model writes is created as an editor would, mode 0666 less the umask, and then given the mode of the file it replaces |

CodeQL does not see the DOS and Windows code, which is behind `#ifdef`.

## Open questions

- HP-UX 11.11 installs in QEMU, but no compiler for its PA-RISC 1.1
  processor was found.
- OpenBSD on SPARC64 passes the batch checks; the TUI could not be run
  there.
- [TODO-DOS.md](TODO-DOS.md) has the open DOS items, and
  [TODO-FEATURES.md](TODO-FEATURES.md) the planned features and build flags.
