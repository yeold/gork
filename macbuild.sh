#!/bin/sh
# Build gork for the classic Mac OS (System 7 on a 68020 or better) with
# the Retro68 cross toolchain, and pack it as OUT/gork-<version>-mac68k.zip:
# the program as MacBinary (gork.bin) and on an HFS disk image (gork.dsk).
# Retro68 is built for 68k into WORK the first time (about 40 minutes and
# 9 GB while it builds, 450 MB kept), from a pinned commit, unless
# RETRO68 names a toolchain built already.  Needs git, gcc and g++, make,
# cmake, bison, flex, texinfo, ruby, the GMP, MPFR, MPC, Boost and zlib
# headers (for Retro68), python3 and zip; run from the source tree:
#     sh macbuild.sh [OUT] [WORK]          (KEEP=1: keep the build tree)
#
# gork's half (main.c, relay.c, notui.c, cJSON, fwlibc.c and
# barebox/libc.c) is built against fwinclude/, with the C library's names
# made gork_* (barebox/rename.py), and joined into one object, gork.o
# (barebox/gork.lds), as for barebox and GRUB; mac/glue.c, the Mac's half,
# is built with Retro68's headers and C library and linked with it.
set -eu
OUT=${1:-out}
WORK=${2:-/tmp/gork-mac}
R68=0cbd3cb918a127a0d30a675b337f8f8e15062786    # Retro68, 2026-10
mkdir -p "$OUT" "$WORK"
OUT=$(cd "$OUT" && pwd)
WORK=$(cd "$WORK" && pwd)
B=$(mktemp -d "$WORK/build.XXXXXX")
[ -n "${KEEP:-}" ] || trap 'rm -rf "$B"' EXIT
mkdir -p "$B/gork" "$B/pack"

# the toolchain: built once (.done when it is whole), and again if it no
# longer runs (a newer system under it, without the libraries it was
# built with).  Built where it stays: its programs know their place.
if [ -z "${RETRO68:-}" ]; then
    RETRO68=$WORK/retro68-$(echo $R68 | cut -c1-10)
    if [ ! -f "$RETRO68/.done" ] ||
       ! "$RETRO68/bin/m68k-apple-macos-gcc" --version >/dev/null 2>&1 ||
       ! "$RETRO68/bin/m68k-apple-macos-ld" --version >/dev/null 2>&1 ||
       ! "$RETRO68/bin/Rez" -h >/dev/null 2>&1; then
        echo "macbuild.sh: building Retro68 (once)" >&2
        rm -rf "$RETRO68"
        git init -q "$B/Retro68"
        (cd "$B/Retro68" &&
         git fetch -q --depth 1 https://github.com/autc04/Retro68.git $R68 &&
         git checkout -q FETCH_HEAD &&
         git submodule -q update --init --recursive --depth 1)
        mkdir -p "$B/r68build"
        (cd "$B/r68build" &&
         "$B/Retro68/build-toolchain.bash" --no-ppc --no-carbon \
             --prefix="$RETRO68" >"$B/r68build.log" 2>&1) ||
            { tail -40 "$B/r68build.log" >&2; exit 1; }
        # the compiler's own programs, stripped: 1.8 GB to a few hundred MB
        find "$RETRO68/bin" "$RETRO68/libexec" -type f -perm -u+x |
            while read -r f; do
                case $(head -c 4 "$f" | od -An -c | tr -d ' ') in
                177ELF) strip "$f" 2>/dev/null || : ;;
                esac
            done
        rm -rf "$B/Retro68" "$B/r68build"
        touch "$RETRO68/.done"
    fi
fi
X=$RETRO68/bin/m68k-apple-macos-

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
for f in glue console roman; do
    ${X}gcc -m68020 -Os -Wall -Wno-format-truncation -Wno-array-bounds -c mac/$f.c -o "$B/$f.o"
done
${X}gcc -m68020 -Wl,--gc-sections "$B/glue.o" "$B/console.o" "$B/roman.o" \
    "$B/gork/gork.o" -o "$B/gork.code.bin"
"$RETRO68/bin/Rez" -I"$RETRO68/RIncludes" --copy "$B/gork.code.bin" \
    mac/gork.r -t APPL -c GoRk \
    -o "$B/pack/gork.bin" --cc "$B/pack/gork.dsk"
cp README.md LICENSE "$B/pack/"
cp docs/build/mac.md "$B/pack/BUILD.md"
z=$OUT/gork-$ver-mac68k.zip
rm -f "$z"
(cd "$B/pack" && zip -qr "$z" .)
echo "$z"
