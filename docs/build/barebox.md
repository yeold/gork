# gork on barebox

[barebox](https://www.barebox.org/) is a boot loader with a shell, file
systems and network drivers, but a network stack of UDP only (DHCP, TFTP,
DNS). gork runs in it as a command, `gork`, on a TCP/IP stack of its own
(`ip.c`, the one the UEFI port uses where the firmware has none), in batch
mode. The release has barebox for QEMU's arm64 `virt` machine with `gork`
built in: `gork-<version>-barebox-arm64.zip`.

## Run, in QEMU

```sh
unzip gork-<version>-barebox-arm64.zip
qemu-system-aarch64 -M virt -cpu cortex-a57 -m 1024 -display none \
    -serial stdio -kernel barebox-dt-2nd.img \
    -netdev user,id=n0 -device virtio-net-device,netdev=n0
```

Stop the autoboot with a key, then, at the `barebox@...:/` prompt:

```sh
echo -o /env/gork.cnf relay = 10.0.2.2:8443
echo -a /env/gork.cnf api_key = sk-ant-...
saveenv                      # keeps /env across boots, where it can
mkdir /tmp/w; cd /tmp/w
gork "write hello.txt with a greeting in it"
```

The settings are `gork.cnf` in `$HOME`, which is `/env` unless set (or the
file `$GORK_CONFIG` names), as on the other systems. gork's TLS is done by a
relay, as everywhere (README); with QEMU's user network the host is
`10.0.2.2`.

## The network

gork takes the first network card, or the one `$GORK_ETH` names (`eth0`),
and gets an address by DHCP, its own, not barebox's `dhcp`. Without DHCP:

```sh
GORK_IP="192.168.1.50/24 192.168.1.1 192.168.1.1"   # address, gateway, DNS
export GORK_IP
```

While gork runs it takes every frame the card receives; barebox's own
network is back when it ends.

## The tools

- The file tools work on barebox's file systems: `/tmp` and `/env` are in
  RAM, and a FAT partition mounted with `mount` can be written (`FS_FAT_WRITE`).
  The working directory is barebox's current one (`cd`).
- `run_command` is barebox's shell, its commands only (there are no
  programs), and has to be allowed in batch mode, as everywhere:
  `GORK_ALLOW_RUN=1; export GORK_ALLOW_RUN`. barebox's shell redirects
  nothing, so gork takes what the command prints from the console.
- barebox renames nothing: a file is copied and the old one removed.
  There are no permissions.

No TUI and no MCP: one program at a time, as on DOS and UEFI.

## Build

```sh
sh bareboxbuild.sh [OUT] [WORK]
```

Needs the aarch64-linux-gnu gcc and binutils, make, bison, flex, libssl-dev,
pkg-config, python3, lz4, lzop and zip. It fetches barebox's source
(checksum pinned), builds gork's half, puts the command in barebox's
`commands/gork/` and builds `multi_v8_defconfig`. `testing/bareboxtest.py`
boots the image in QEMU and runs the checks against `testing/fakeapi.c`.

### In another barebox

For a board's own barebox, take `commands/gork/` from a build of
`bareboxbuild.sh` (`glue.c`, `bb.h`, `gork.o_shipped` and its `Makefile`),
put it in that barebox's tree with `obj-y += gork/` in `commands/Makefile`,
and build. `gork.o` is for arm64 and barebox v2026.09; for another version
the calls of `glue.c` may need changes. The board needs a network driver
that barebox has, and about 1.5 MB of malloc space beyond its own.

## How it works

gork's half (`main.c`, `relay.c`, cJSON, `ip.c`, `fwlibc.c` and
`barebox/libc.c`) is built against the standard headers in `fwinclude/`,
with every C library name made `gork_NAME` (`barebox/rename.py`), so that
it meets none of barebox's own `printf`, `open` and the rest, and is joined
into one object (`barebox/gork.lds`). `barebox/glue.c`, built as barebox
builds its commands, is the command and does what touches barebox through
the calls of `barebox/bb.h`: files, the environment, the console, the
shell, the clock, and the network card, whose frames it takes from the
driver before barebox's own stack sees them (`rx_preprocessor`).

A command, unlike a program, keeps its variables from one run to the next:
`glue.c` saves gork's before the first run and puts them back before each
later one, runs gork on a stack of its own (barebox's is 32 KB), and comes
back from `exit()` by `longjmp`; what gork allocated is freed then.
