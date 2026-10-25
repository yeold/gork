#!/bin/sh
# Cross-build gork for UEFI, started from the UEFI shell: gorkx64.efi for
# x86-64 and gorkaa64.efi for arm64, packed as OUT/gork-<version>-uefi.zip.
# Built with clang and lld against gork's own C library (uefi/), so
# nothing is fetched.  Needs clang, lld and zip; run from the source tree:
#     sh uefibuild.sh [OUT] [WORK]
#     ARCHS=x64 sh uefibuild.sh            # only one
set -eu
OUT=${1:-out}
WORK=${2:-/tmp/gork-uefi}
ARCHS=${ARCHS:-"x64 aa64"}
CC=${CC:-clang}
LD=${LD:-ld.lld}

rm -rf "$WORK/pack"
mkdir -p "$OUT" "$WORK/pack"
OUT=$(cd "$OUT" && pwd)
WORK=$(cd "$WORK" && pwd)

# The Windows targets give the PE/COFF objects UEFI takes, and on x86-64
# its calling convention; what they predefine for Windows is taken back.
for a in $ARCHS; do
    case $a in
    x64)  triple=x86_64-unknown-windows  machine=x64 extra=-mno-red-zone ;;
    aa64) triple=aarch64-unknown-windows machine=arm64 extra= ;;
    *)    echo "uefibuild.sh: no such architecture: $a" >&2; exit 2 ;;
    esac
    CFLAGS="--target=$triple $extra -ffreestanding -nostdlibinc -fno-builtin
        -fshort-wchar -mno-stack-arg-probe -fno-stack-protector
        -U_WIN32 -U_WIN64 -U_MSC_VER -DGORK_EFI -Iuefi/include -Os -Wall -W
        -DGORK_VERSION=\"$(sh version.sh)\""
    mkdir -p "$WORK/$a"
    for f in main relay notui cJSON/cJSON; do
        $CC $CFLAGS -std=gnu89 -pedantic -c $f.c -o "$WORK/$a/$(basename $f).o"
    done
    for f in uefi/libc fwlibc uefi/net uefi/card ip; do
        $CC $CFLAGS -std=gnu99 -c $f.c -o "$WORK/$a/$(basename $f).o"
    done
    $LD -flavor link -machine:$machine -subsystem:efi_application \
        -entry:efi_main -nodefaultlib -out:"$WORK/pack/gork$a.efi" \
        "$WORK/$a"/main.o "$WORK/$a"/relay.o "$WORK/$a"/notui.o \
        "$WORK/$a"/cJSON.o "$WORK/$a"/libc.o "$WORK/$a"/fwlibc.o "$WORK/$a"/net.o \
        "$WORK/$a"/card.o \
        "$WORK/$a"/ip.o
done

ver=$(sed -n 's/^AC_INIT(\[gork\], \[\(.*\)\])/\1/p' configure.ac)
cp README.md LICENSE "$WORK/pack/"
cp docs/build/uefi.md "$WORK/pack/BUILD.md"
rm -f "$OUT/gork-$ver-uefi.zip"
(cd "$WORK/pack" && zip -q "$OUT/gork-$ver-uefi.zip" gork*.efi README.md \
    LICENSE BUILD.md)
echo "$OUT/gork-$ver-uefi.zip"
