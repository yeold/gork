#!/bin/sh
# Build gork for the classic Mac OS (System 7 on a 68020 or better) with
# the Retro68 cross toolchain, and pack it as OUT/gork-<version>-mac68k.zip:
# the program as MacBinary (gork.bin) and on an HFS disk image (gork.dsk).
# Needs Retro68 built for 68k (RETRO68: its toolchain directory), python3
# and zip; run from the source tree:
#     sh macbuild.sh [OUT]                 (KEEP=1: keep the build tree)
#
# gork's half (main.c, relay.c, notui.c, cJSON, fwlibc.c and
# barebox/libc.c) is built against fwinclude/, with the C library's names
# made gork_* (barebox/rename.py), and joined into one object, gork.o
# (barebox/gork.lds), as for barebox and GRUB; mac/glue.c, the Mac's half,
# is built with Retro68's headers and C library and linked with it.
set -eu
OUT=${1:-out}
RETRO68=${RETRO68:-/opt/Retro68/toolchain}
X=$RETRO68/bin/m68k-apple-macos-
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
B=$(mktemp -d "${TMPDIR:-/tmp}/gork-mac.XXXXXX")
[ -n "${KEEP:-}" ] || trap 'rm -rf "$B"' EXIT
mkdir -p "$B/gork" "$B/pack"

ver=$(sed -n 's/^AC_INIT(\[gork\], \[\(.*\)\])/\1/p' configure.ac)
gver=$(sh version.sh)

# gork's half: a 68020 (System 7's 32-bit Macs), the FPU left alone
# (cJSON's doubles in software, as many of those Macs have none)
python3 barebox/rename.py "${X}gcc -E -Imac/include" "$B/gork/rename.h"
CFLAGS="-ffreestanding -fno-builtin -nostdinc -isystem $(${X}gcc -print-file-name=include)
    -Ifwinclude -Imac/include -include $B/gork/rename.h -DGORK_MAC
    -DGORK_VERSION=\"$gver\" -m68020 -fno-common -fno-strict-aliasing
    -fno-delete-null-pointer-checks -fno-math-errno -Os -Wall -W"
for f in main relay notui cJSON/cJSON; do
    ${X}gcc $CFLAGS -std=gnu89 -pedantic -c $f.c -o "$B/gork/$(basename $f).o"
done
for f in fwlibc barebox/libc; do
    ${X}gcc $CFLAGS -std=gnu99 -c $f.c -o "$B/gork/$(basename $f).o"
done
${X}ld -r -T barebox/gork.lds -o "$B/gork/gork.o" \
    "$B/gork"/main.o "$B/gork"/relay.o "$B/gork"/notui.o \
    "$B/gork"/cJSON.o "$B/gork"/fwlibc.o "$B/gork"/libc.o

# the Mac's half, and the program
${X}gcc -m68020 -Os -Wall -Wno-format-truncation -Wno-array-bounds -c mac/glue.c -o "$B/glue.o"
${X}g++ -m68020 -Wl,--gc-sections "$B/glue.o" "$B/gork/gork.o" -o "$B/gork.code.bin" -lRetroConsole
"$RETRO68/bin/Rez" -I"$RETRO68/RIncludes" --copy "$B/gork.code.bin" \
    mac/gork.r -t APPL -c GoRk \
    -o "$B/pack/gork.bin" --cc "$B/pack/gork.dsk"
cp README.md LICENSE "$B/pack/"
cp docs/build/mac.md "$B/pack/BUILD.md" 2>/dev/null || :
z=$OUT/gork-$ver-mac68k.zip
rm -f "$z"
(cd "$B/pack" && zip -qr "$z" .)
echo "$z"
