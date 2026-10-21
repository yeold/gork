# Building gork for UEFI

Back to [all systems](../BUILD.md).

Experimental. gork for UEFI is a UEFI application, `gorkx64.efi` for
x86-64 and `gorkaa64.efi` for arm64: no operating system, started from
the UEFI shell, using the firmware's own TCP/IP stack. Batch mode only.
Tested with EDK2's firmware for QEMU (OVMF and AAVMF 2025.02, UEFI 2.70)
and its UEFI shell 2.2. Below, `gork.efi` stands for either.

## Build

On Linux, with clang and lld; nothing is downloaded, as the C library is
gork's own (`uefi/`):

```sh
sh uefibuild.sh
```

leaves `gork-<version>-uefi.zip` in `out/`, with both programs
(`ARCHS=x64` or `ARCHS=aa64` builds one). By hand, each file is compiled
for the Windows target, which gives the PE/COFF objects UEFI takes (and on
x86-64 its calling convention), with what that target predefines taken
back; for arm64 the target is `aarch64-unknown-windows`, without
`-mno-red-zone`, and the link takes `-machine:arm64`:

```sh
CFLAGS="--target=x86_64-unknown-windows -ffreestanding -nostdlibinc
    -fno-builtin -fshort-wchar -mno-red-zone -mno-stack-arg-probe
    -fno-stack-protector -U_WIN32 -U_WIN64 -U_MSC_VER -DGORK_EFI
    -Iuefi/include -Os"
for f in main relay notui cJSON/cJSON uefi/libc uefi/net; do
    clang $CFLAGS -c $f.c -o $(basename $f).o
done
ld.lld -flavor link -subsystem:efi_application -entry:efi_main \
    -nodefaultlib -out:gork.efi main.o relay.o notui.o cJSON.o libc.o net.o
```

`uefi/libc.c` is the C library: files through the shell's own file
calls, the console, the environment (the shell's variables), time,
`printf` and the rest of what main.c, relay.c and cJSON call.
`uefi/net.c` gives BSD sockets on the firmware's TCP4 and DNS4
protocols, so relay.c takes the same path as on Unix.

## Run

Put `gork.efi` and a `gork.cnf` on a FAT disk the firmware can read: the
EFI system partition, or a USB stick. In the UEFI shell:

```
Shell> fs0:
FS0:\> cd work
FS0:\work\> fs0:\gork.efi "what is in this directory?"
```

The config is `gork.cnf` beside `gork.efi`, and gork's directory `_gork`
beside it, as on DOS; a shell variable `HOME` moves both. Environment
overrides are shell variables: `set GORK_ALLOW_RUN 1` turns on
`run_command`, `set -d` takes one away again. The shell writes files as
UTF-16, so a `gork.cnf` or `GORK.md` made with `echo ... > file` or `edit`
is read all the same.

A PC whose firmware has no shell of its own can boot one: EDK2's
`Shell.efi` (Debian's `efi-shell-x64` package has it as `shellx64.efi`)
copied to `\EFI\BOOT\BOOTX64.EFI` on a FAT USB stick.

## The network

gork uses the firmware's TCP/IP when it has one, and its own otherwise
(below). With the firmware's, gork uses the first network card that has
an address. A card without one
gets DHCP started, as `ifconfig -s eth0 dhcp` would, and gork waits up to
20 seconds; for a static address, set it in the shell first:

```
ifconfig -s eth0 static 192.168.1.50 255.255.255.0 192.168.1.1
```

A relay given by name is looked up with the firmware's DNS, through the
servers DHCP gave. The firmware needs its network stack: on a PC that is
often a setup option ("Network stack", "PXE"), and the card needs a UEFI
driver, in the firmware or loaded with `load`. OVMF has the stack, but
starts it only with a random number generator in the machine: a virtio
one on x86-64, and on arm64 a CPU with the RNDR instruction (`-cpu max`),
as the virtio one comes too late there:

```sh
qemu-system-x86_64 -machine q35 -m 512 \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
    -drive if=pflash,format=raw,file=OVMF_VARS_4M.fd \
    -drive format=raw,file=esp.img \
    -device virtio-rng-pci -nic user,model=virtio-net-pci
```

```sh
qemu-system-aarch64 -M virt -cpu max -m 1024 \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/AAVMF/AAVMF_CODE.fd \
    -drive if=pflash,format=raw,file=AAVMF_VARS.fd \
    -drive if=none,id=esp,format=raw,file=esp.img -device virtio-blk-pci,drive=esp \
    -nic user,model=virtio-net-pci
```

Booted straight from a disk, firmware may leave the network drivers
unconnected; gork connects them itself when it finds no TCP.

**Without the firmware's TCP/IP** -- U-Boot, or a PC with its network
stack turned off but a driver for its card -- gork uses a TCP/IP stack of
its own (`uefi/ip.c`) on the card itself: IPv4, DHCP, DNS and one TCP
connection at a time, enough for the relay. It answers ARP and ping for
its address. For a static address, without DHCP, set a shell variable:

```
set GORK_IP "192.168.1.50/24 192.168.1.1 192.168.1.1"
```

(address and prefix, gateway, DNS server; the last two may be left out).
gork does DHCP afresh at each start there, which takes a moment.

**U-Boot** (2025.01, `qemu_arm64`) starts EDK2's shell from a disk through
its own UEFI layer (put the shell at `\EFI\BOOT\BOOTAA64.EFI`), and
`gorkaa64.efi` runs there on its own TCP/IP. U-Boot renames no files, so a
`write_file` there copies the new file over the old one instead: not
atomic.

```sh
qemu-system-aarch64 -M virt -cpu max -m 1024 \
    -bios /usr/lib/u-boot/qemu_arm64/u-boot.bin \
    -drive if=none,id=esp,format=raw,file=esp.img -device virtio-blk-pci,drive=esp \
    -nic user,model=virtio-net-pci
```

## What UEFI lacks, and what gork does about it

| | On UEFI | So |
|---|---|---|
| Processes | one program at a time | `run_command` goes to the shell, which runs the line in a shell of its own; output and errors come back, but there is no timeout; no MCP servers |
| Curses | none | batch mode only: `notui.c` takes the place of `tui.c` |
| Paths | `fs0:\dir\file`, any case | gork shows them as `fs0:/dir/file`; either slash works, and names are compared without case |
| Text files | CR LF, and UTF-16 from the shell | text mode, as on DOS: the model sees bare newlines, files keep CR LF; a file that starts with the UTF-16 mark is read as UTF-8 |
| File modes | none on FAT | the `api_key` permission check is skipped |
| Signals | none | Ctrl-C (the shell's break) ends gork; the request timeout is a deadline that the read polls |
| The clock | the firmware's, with no reliable time zone | taken to be UTC |

## Testing

`python3 testing/uefitest.py ZIP FAKEAPI [MACHINE]` boots OVMF (`x64`),
AAVMF (`aa64`), OVMF without its network stack (`x64-own`, so gork's own
TCP/IP) or U-Boot (`uboot`) in QEMU with the UEFI shell, gork and the
checks on a FAT image, against the fake
API (`testing/fakeapi.c`, built for the host): a reply, the file tools,
`run_command`, paths outside the working directory, a subdirectory, a
UTF-16 config, the request timeout and the exit status. It needs
QEMU, the firmware and a UEFI shell for the architecture (Debian's
`qemu-system-x86`, `ovmf` and `efi-shell-x64`, or `qemu-system-arm`,
`qemu-efi-aarch64` and `efi-shell-aa64`; `u-boot-qemu` for U-Boot), mtools
and dosfstools. Jenkins runs all four on every build.
