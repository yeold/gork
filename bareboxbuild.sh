#!/bin/sh
# Build barebox with the `gork` command in it, for QEMU's arm64 `virt`
# machine (barebox's multi_v8_defconfig), and pack the image as
# OUT/gork-<version>-barebox-arm64.zip.  barebox's source is fetched into
# WORK, checksum pinned.  Needs the aarch64-linux-gnu gcc and binutils,
# make, bison, flex, libssl-dev, pkg-config, python3, lz4, lzop and zip;
# run from the source tree:
#     sh bareboxbuild.sh [OUT] [WORK]        (KEEP=1: keep the build tree)
#
# gork's half (main.c, relay.c, notui.c, cJSON, ip.c, fwlibc.c and
# barebox/libc.c) is built here against fwinclude/, with the C library's
# names made gork_* (barebox/rename.py), and joined into one object,
# gork.o (barebox/gork.lds); barebox builds barebox/glue.c, the command,
# beside it as commands/gork/.  barebox/bb.h says why there are two.
set -eu
OUT=${1:-out}
WORK=${2:-/tmp/gork-barebox}
BV=2026.09.0
CROSS=${CROSS:-aarch64-linux-gnu-}
mkdir -p "$OUT" "$WORK"
OUT=$(cd "$OUT" && pwd)
WORK=$(cd "$WORK" && pwd)

f=$WORK/barebox-$BV.tar.gz
[ -f "$f" ] || { curl -fsSL -o "$f.$$" "https://github.com/barebox/barebox/archive/refs/tags/v$BV.tar.gz" && mv "$f.$$" "$f"; }
echo "e0cfed9e4bf5b271cb2bb6bfa3a5f2b925c1d0bc16c84925238716f59ef3babb  $f" | sha256sum -c - >/dev/null
# the build in a directory of its own: two builds may share WORK
B=$(mktemp -d "$WORK/build.XXXXXX")
# a few hundred MB: gone at the end, failed or not; the download stays
[ -n "${KEEP:-}" ] || trap 'rm -rf "$B"' EXIT
src=$B/barebox-$BV
tar xzf "$f" -C "$B"
mkdir -p "$B/gork"

ver=$(sed -n 's/^AC_INIT(\[gork\], \[\(.*\)\])/\1/p' configure.ac)
gver=$(sh version.sh)          # what --version says

# gork's half: barebox's flags for arm64 (make V=1 shows them), but the
# FP registers allowed (cJSON's numbers are doubles; barebox turns the FPU
# on and has no interrupts to save them for), and fwinclude/ for headers.
python3 barebox/rename.py "${CROSS}gcc -E" "$B/gork/rename.h"
CFLAGS="-ffreestanding -fno-builtin -nostdinc -isystem $(${CROSS}gcc -print-file-name=include)
    -Ifwinclude -include $B/gork/rename.h -DGORK_BAREBOX
    -DGORK_VERSION=\"$gver\" -mlittle-endian -mabi=lp64 -march=armv8-a
    -mstrict-align -fshort-wchar -funsigned-char -fno-common -fPIE
    -fno-stack-protector -mbranch-protection=none -fno-strict-aliasing
    -fno-delete-null-pointer-checks -fno-math-errno -Os -Wall -W"
for f in main relay notui cJSON/cJSON; do
    ${CROSS}gcc $CFLAGS -std=gnu89 -pedantic -c $f.c -o "$B/gork/$(basename $f).o"
done
for f in ip fwlibc barebox/libc; do
    ${CROSS}gcc $CFLAGS -std=gnu99 -c $f.c -o "$B/gork/$(basename $f).o"
done
# one object, its variables together; libgcc for what the compiler calls
${CROSS}ld -r -T barebox/gork.lds -o "$B/gork/gork.o" \
    "$B/gork"/main.o "$B/gork"/relay.o "$B/gork"/notui.o \
    "$B/gork"/cJSON.o "$B/gork"/ip.o "$B/gork"/fwlibc.o \
    "$B/gork"/libc.o "$(${CROSS}gcc -print-libgcc-file-name)"

# barebox's half, and the command into barebox
mkdir -p "$src/commands/gork"
cp barebox/glue.c barebox/bb.h "$src/commands/gork/"
cp "$B/gork/gork.o" "$src/commands/gork/gork.o_shipped"
printf 'obj-y += glue.o gork.o\n' >"$src/commands/gork/Makefile"
printf 'obj-y += gork/\n' >>"$src/commands/Makefile"
make -s -C "$src" ARCH=arm CROSS_COMPILE="$CROSS" multi_v8_defconfig
make -s -C "$src" ARCH=arm CROSS_COMPILE="$CROSS" -j"$(nproc)"

d=$B/pack
mkdir "$d"
cp "$src/images/barebox-dt-2nd.img" README.md LICENSE "$d/"
cp docs/build/barebox.md "$d/BUILD.md"
rm -f "$OUT/gork-$ver-barebox-arm64.zip"
(cd "$d" && zip -q "$OUT/gork-$ver-barebox-arm64.zip" *)
echo "$OUT/gork-$ver-barebox-arm64.zip"
