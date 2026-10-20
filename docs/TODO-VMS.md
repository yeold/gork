# TODO: test machines anyone can rebuild

Every system in the README table that was tried in an emulator ran in a
VM set up by hand on one machine. The scripts that drive those VMs exist,
but outside this repository and tied to that machine: fixed paths, a
private podman store, and container images built there without a
`Containerfile` kept. Only [testing/oldvm.sh](../testing/oldvm.sh)
(Debian 1.3) can be rebuilt from here. The aim: one directory per VM,
under `testing/vm/<os>/`, so that someone else can set up the same
environment on their own machine and run the same checks.

## What each VM directory needs

- `fetch.sh`: downloads the media from where it was found (archive.org,
  WinWorld, the vendor) and checks its checksum. The media itself stays out
  of the repository: most of it is someone else's software.
- A `Containerfile` for the emulator, shared where VMs use the same one:
  QEMU from Debian (x86, PowerPC, SPARC, PA-RISC, m68k) and 86Box with its
  ROM set.
- `up.sh`: starts the VM with paths relative to its directory, its
  disks, port forwards and serial or monitor sockets.
- The install, as a script where it is driven by keys or floppy swaps, and
  as step-by-step notes in a `README.md` where the installer is graphical.
- `provision.sh`: copies the gork source in, builds it the way the build
  guide in [docs/build/](build/) says, and runs the fake API checks
  ([testing/fakeapi.c](../testing/fakeapi.c), `fakemcp.py`) against it.

The helpers already written (serial loggers, telnet and ssh runners,
uuencode uploaders, screen readers through QEMU's monitor or pyte) are
generic and can be shared from `testing/vm/lib/`.

## The VMs, by how well they script

| VM | Emulator | Install |
|---|---|---|
| 9front | QEMU x86-64 | Ready-made qcow2 from 9front.org: fully scriptable |
| NetBSD 11.0 virt68k | QEMU m68k `virt` | sysinst over the serial line: scriptable |
| Debian m68k | QEMU `q800` | Ready-made image: scriptable |
| Dell Unix SVR4 | QEMU (install), 86Box (network) | Floppy swaps through the QEMU monitor: scriptable; the WD8013 driver setup is a few commands |
| AIX 4.3.3 | Patched QEMU 3.0 (`40p`), from the archive.org item `aix433_qemu` | Ready-made image: scriptable, once the patched QEMU builds from source |
| HP-UX 10.20 and 11.11 | QEMU `B160L` | Serial installer: scriptable, slow |
| SunOS 4.1.4 | QEMU SPARCstation 5 | Serial installer and PROM commands: scriptable |
| Minix 2.0.4 | QEMU | Floppy and serial: scriptable |
| QNX 6.2.1 | QEMU | Graphical installer: notes |
| BeOS R5 | QEMU | Graphical, with a patched driver: notes |
| OS/2 Warp 3 and 4.52 | QEMU | Graphical, with CONFIG.SYS changes: notes |
| OPENSTEP 4.2, NeXTSTEP 3.3 | 86Box | Graphical and floppy driven: notes |
| AIX PS/2 1.3 | 86Box (PS/2 Model 80) | Floppies: notes |
| Mac OS X 10.4 | QEMU `mac99` | Graphical, driven by screenshots and a calibrated mouse: notes |
| DOS, Windows 1.x to XP | QEMU, DOSBox-X | Partly in [testing/](../testing/) already (`dostest.py`, `wintest.py`); the images themselves: notes |

UnixWare 2.1.2 and QNX 4.25 stopped at a licence key and need no
directory.

Where the notes for each VM are now: the progress log kept with the VMs
(install steps, pitfalls, the fixes the guests needed). It is the source
for each `README.md`.
