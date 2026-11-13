# gork on GRUB

[GRUB](https://www.gnu.org/software/grub/) is a boot loader with a shell,
file systems it reads but does not write, and a network stack of its own.
gork runs in it as a command, `gork`, a module of GRUB's, in batch mode,
on gork's own TCP/IP (`ip.c`, the one the UEFI and barebox ports use) over
GRUB's network card drivers. The release has GRUB 2.14 for UEFI with
`gork` built in, and the module alone: `gork-<version>-grub-x64.zip` for
x86-64 PCs (`BOOTX64.EFI`) and `gork-<version>-grub-aa64.zip` for arm64
(`BOOTAA64.EFI`); and `gork-<version>-grub-i386.zip` for a BIOS PC
(`gork.pxe`), which is PXE only -- see below.

## Run, in QEMU

```sh
unzip gork-<version>-grub-x64.zip -d esp
cat >esp/EFI/BOOT/gork.cnf <<EOF
relay = 10.0.2.2:8443
api_key = sk-ant-...
EOF
mkfs.fat -C -F 16 esp.img 65536 && mcopy -i esp.img -s esp/EFI ::/
cp /usr/share/OVMF/OVMF_VARS_4M.fd vars.fd
qemu-system-x86_64 -machine q35 -m 512 -display none -serial stdio \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
    -drive if=pflash,format=raw,file=vars.fd \
    -drive if=none,id=esp,format=raw,file=esp.img \
    -device ide-hd,drive=esp,bus=ide.0,bootindex=0 \
    -nic user,model=virtio-net-pci
```

On arm64, the same disk with the `aa64` package, in QEMU's `virt` machine
under AAVMF:

```sh
cp /usr/share/AAVMF/AAVMF_VARS.fd vars.fd
qemu-system-aarch64 -M virt -cpu cortex-a72 -m 1024 -display none -serial stdio \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/AAVMF/AAVMF_CODE.fd \
    -drive if=pflash,format=raw,file=vars.fd \
    -drive if=none,id=esp,format=raw,file=esp.img \
    -device virtio-blk-pci,drive=esp,bootindex=0 \
    -netdev user,id=n -device virtio-net-pci,netdev=n,romfile=
```

With no `grub.cfg`, GRUB stops at its prompt:

```
grub> gork "write hello.txt with a greeting in it"
grub> cat (gork)/hello.txt
```

The settings are `gork.cnf` in `$HOME`, which is GRUB's `$prefix` (where
`grub.cfg` is, `/EFI/BOOT` here) unless set, or the file `$GORK_CONFIG`
names. gork's TLS is done by a relay, as everywhere (README); with QEMU's
user network the host is `10.0.2.2`.

## Files

GRUB writes no file system, so gork writes in `(gork)`, a device of its
own in RAM that GRUB's commands read as any other: `ls (gork)/`,
`cat (gork)/x`, `configfile (gork)/x.cfg`. It lasts until GRUB boots
something or the machine restarts.

The working directory is `$GORK_DIR`, or `(gork)/` if unset; one in
`(gork)` that is not there yet is made. Elsewhere gork can read and list
but not write: `set GORK_DIR=$prefix` lets it read `grub.cfg` and
the files beside it, and each write there fails with "read-only file
system". The `allow` setting adds directories, as everywhere.

## The network

gork takes the first network card (`net_ls_cards`), or the one
`$GORK_ETH` names (`efinet0`), and gets an address by DHCP, its own, not
GRUB's `net_dhcp`. Without DHCP:

```
set GORK_IP="192.168.1.50/24 192.168.1.1 192.168.1.1"   # address, gateway, DNS
```

Firmware that boots from a disk may not start the card's driver (OVMF
does not); gork then starts the drivers of every device and has GRUB's
`efinet` look for cards again.

## The tools

- `run_command` is GRUB's shell, its commands only, and has to be
  allowed in batch mode, as everywhere: `set GORK_ALLOW_RUN=1`. What the
  command prints, its errors too, is taken from the console.
- There are no permissions, and no renaming out of `(gork)`.

No TUI and no MCP: one program at a time, as on DOS, UEFI and barebox.

## A BIOS PC: PXE only

On a BIOS PC GRUB has one network driver, the PXE ROM's
(`grub-core/net/drivers/i386/pc/pxe.c`, the only one built for
`i386-pc`), and the ROM is in the card, not in GRUB: a machine booted
from its disk has no card at all. So `gork-<version>-grub-i386.zip` is
`gork.pxe`, a core image built by `grub-mkimage -O i386-pc-pxe` for a
card's ROM to fetch. There is no floppy or disk image that would give
BIOS GRUB a card.

Boot it in QEMU, with a card whose ROM knows PXE and QEMU's own TFTP
server (no `grub.cfg` needed: GRUB stops at its prompt, on the serial
line `grub.cfg` asks for, or on VGA):

```sh
mkdir -p tftp/boot/grub
unzip gork-<version>-grub-i386.zip -d tftp
cat >tftp/boot/grub/gork.cnf <<EOF
relay = 10.0.2.2:8443
api_key = sk-ant-...
EOF
qemu-system-i386 -m 256 -display none -serial stdio \
    -netdev user,id=n0,tftp=tftp,bootfile=gork.pxe \
    -device rtl8139,netdev=n0 -boot n
```

A scripted run needs a `grub.cfg` (`testing/grubtest.py` writes one), and
it has to turn the serial console on itself, since on BIOS GRUB's console
is VGA: `serial --unit=0 --speed=115200` then
`terminal_output serial console`, both of which are modules in the image.
The `(pxe)` device is GRUB's TFTP "file system": a file is fetched when it
is opened, and there are no directories there, so gork's working directory
is `(gork)` as usual and `$prefix` is `(pxe)/boot/grub`.

Two things the UEFI builds never showed, both fixed in `glue.c`:

- GRUB's TFTP file system answers a directory listing for **any** path
  with success, so a missing file read as an existing empty directory
  (`EISDIR`) instead of not being there (`ENOENT`), and gork stopped
  before its first request, on a `GORK.md` it did not need. A net device
  is no longer asked whether something is a directory.
- TFTP reports a file the server does not have as `GRUB_ERR_IO` with the
  server's message, not as `GRUB_ERR_FILE_NOT_FOUND`, so the same missing
  file was a hard error; `gerr()` reads the message.

The package also keeps `gork-disk.img`, a plain `i386-pc` core image with
`boot.img` and `grub-bios-setup`, for `gork`'s file tools on a machine
booted from disk. No card is reachable from it.

## In a GRUB of one's own

`gork.mod` is for GRUB 2.14 on UEFI (`x86_64-efi`, or `arm64-efi` in the
`aa64` package; `i386-pc` in the `i386` one), as GNU releases it. Put it with GRUB's other modules
(`$prefix/x86_64-efi/` or `$prefix/arm64-efi/`) and
`insmod gork`; GRUB loads `net`, `normal`, `datetime` and `setjmp` with
it. It needs a network card driver (`efinet`) loaded too. Distributions
patch their GRUB, and under Secure Boot their GRUB loads no module
that is not signed into it, so it may refuse gork.mod: the
`BOOTX64.EFI` or `BOOTAA64.EFI` of the package is the sure route.

## Build

```sh
sh grubbuild.sh [OUT] [WORK] [x64|aa64|i386|of]
```

On x86-64; `aa64` with the `aarch64-linux-gnu-` gcc and binutils too
(`CROSS` names another prefix), and `i386` with the host's gcc and
`gcc-multilib` (`-m32`); `of` (PowerPC Open Firmware) with
`powerpc-linux-gnu-` and still unproven. `i386` builds 32-bit code with
GRUB's own calling convention (`-mrtd -mregparm=3`) and no MMX or SSE,
as GRUB builds `i386-pc`; `grub/glue.c` matches it for every `bb_*` and
`card_*` call. Needs gcc and binutils, make, bison, flex, gawk, autoconf,
autoconf-archive, automake, autopoint (gettext), python3, xz and zip. It
fetches GRUB's source (checksum pinned), builds gork's half, adds the
module to GRUB's `grub-core/Makefile.core.def`, builds GRUB for
`x86_64-efi` or `arm64-efi` and makes `BOOTX64.EFI` or `BOOTAA64.EFI`
with `grub-mkimage`. `testing/grubtest.py` boots it in QEMU under OVMF or
AAVMF, or the `i386` package over PXE with QEMU's TFTP server, and runs
the checks against `testing/fakeapi.c`.

## How it works

gork's half is the barebox port's: `main.c`, `relay.c`, cJSON, `ip.c`,
`fwlibc.c` and `barebox/libc.c`, built against `fwinclude/` with every C
library name made `gork_NAME` (`barebox/rename.py`) and joined into one
object (`barebox/gork.lds`). `grub/glue.c`, built as GRUB builds its
modules, is the command and gives the calls of `barebox/bb.h` on GRUB's:
its files, its environment, its console and shell, its clock, `(gork)`,
and the network card, whose frames it takes from GRUB's driver.

As on barebox, a command keeps its variables from one run to the next:
`glue.c` saves gork's before the first run and puts them back before each
later one, runs gork on a stack of its own, and comes back from `exit()`
by `longjmp`.
