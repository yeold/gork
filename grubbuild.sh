#!/bin/sh
# Build GRUB for UEFI with the `gork` command in it, and the module
# gork.mod for a GRUB 2.14 of one's own, and pack them as
# OUT/gork-<version>-grub-ARCH.zip: ARCH x64 (x86-64, the default) or
# aa64 (arm64, with the aarch64-linux-gnu gcc and binutils, CROSS).
# GRUB's source is fetched into WORK, checksum pinned.  Needs an x86-64
# gcc and binutils, make, bison, flex, gawk, autoconf, automake, autopoint
# (gettext), python3, xz and zip; run from the source tree:
#     sh grubbuild.sh [OUT] [WORK] [ARCH]   (KEEP=1: keep the build tree)
#
# gork's half (main.c, relay.c, notui.c, cJSON, ip.c, fwlibc.c and
# barebox/libc.c) is built here against fwinclude/, with the C library's
# names made gork_* (barebox/rename.py), and joined into one object,
# gork.o (barebox/gork.lds); GRUB builds grub/glue.c, the command, beside
# it as the module gork, as it builds its own.  barebox/bb.h says why
# there are two halves.
set -eu
OUT=${1:-out}
WORK=${2:-/tmp/gork-grub}
ARCH=${3:-x64}
GV=2.14
case $ARCH in
x64)  CROSS=; target=x86_64; fmt=x86_64-efi; boot=BOOTX64.EFI
      # GRUB's code model for x86-64 modules (large: a module may be
      # loaded anywhere) and no red zone (the firmware's interrupts use
      # the same stack), but SSE allowed (cJSON's numbers are doubles;
      # UEFI turns it on)
      MFLAGS="-m64 -mcmodel=large -mno-red-zone -fcf-protection=none" ;;
aa64) CROSS=${CROSS:-aarch64-linux-gnu-}; target=aarch64-linux-gnu
      fmt=arm64-efi; boot=BOOTAA64.EFI
      # the large code model too, but the FP registers allowed (UEFI
      # turns them on, and GRUB does not touch them)
      MFLAGS="-mcmodel=large -mstrict-align -mbranch-protection=none
          -mno-outline-atomics" ;;
*)    echo "grubbuild.sh: ARCH is x64 or aa64" >&2; exit 2 ;;
esac
mkdir -p "$OUT" "$WORK"
OUT=$(cd "$OUT" && pwd)
WORK=$(cd "$WORK" && pwd)
[ "$(uname -m)" = x86_64 ] || { echo "grubbuild.sh: builds on x86-64 only" >&2; exit 2; }

f=$WORK/grub-$GV.tar.xz
[ -f "$f" ] || { curl -fsSL -o "$f.$$" "https://ftp.gnu.org/gnu/grub/grub-$GV.tar.xz" && mv "$f.$$" "$f"; }
echo "bc8d3c73535b8838d8c8e2654d73edc4e6ae8c8acdb45d5df5dc9a1547446d43  $f" | sha256sum -c - >/dev/null
# the build in a directory of its own: two builds may share WORK
B=$(mktemp -d "$WORK/build.XXXXXX")
# a few hundred MB: gone at the end, failed or not; the download stays
[ -n "${KEEP:-}" ] || trap 'rm -rf "$B"' EXIT
src=$B/grub-$GV
tar xJf "$f" -C "$B"
mkdir -p "$B/gork" "$B/build"

ver=$(sed -n 's/^AC_INIT(\[gork\], \[\(.*\)\])/\1/p' configure.ac)
gver=$(sh version.sh)          # what --version says

# gork's half: GRUB's flags for modules, as above, and fwinclude/ for
# headers.
python3 barebox/rename.py "${CROSS}gcc -E" "$B/gork/rename.h"
CFLAGS="-ffreestanding -fno-builtin -nostdinc -isystem $(${CROSS}gcc -print-file-name=include)
    -Ifwinclude -include $B/gork/rename.h -DGORK_GRUB
    -DGORK_VERSION=\"$gver\" $MFLAGS -fno-pic
    -fno-PIE -fno-stack-protector -fno-common -fno-strict-aliasing
    -fno-asynchronous-unwind-tables -fno-unwind-tables
    -fno-delete-null-pointer-checks -fno-math-errno -Os -Wall -W"
for f in main relay notui cJSON/cJSON; do
    ${CROSS}gcc $CFLAGS -std=gnu89 -pedantic -c $f.c -o "$B/gork/$(basename $f).o"
done
for f in ip fwlibc barebox/libc; do
    ${CROSS}gcc $CFLAGS -std=gnu99 -c $f.c -o "$B/gork/$(basename $f).o"
done
# one object, its variables together
${CROSS}ld -r -T barebox/gork.lds -o "$B/gork/gork.o" \
    "$B/gork"/main.o "$B/gork"/relay.o "$B/gork"/notui.o \
    "$B/gork"/cJSON.o "$B/gork"/ip.o "$B/gork"/fwlibc.o \
    "$B/gork"/libc.o

# GRUB's half: the command as a module of GRUB's, with gork.o linked in
mkdir -p "$src/grub-core/commands/gork"
cp grub/glue.c "$src/grub-core/commands/gork/"
cp "$B/gork/gork.o" "$src/grub-core/commands/gork/"
cat >>"$src/grub-core/Makefile.core.def" <<EOF

module = {
  name = gork;
  common = commands/gork/glue.c;
  ldadd = '\$(srcdir)/commands/gork/gork.o';
  enable = x86_64_efi;
  enable = arm64_efi;
};
EOF
(cd "$src" && ./autogen.sh >/dev/null)
(cd "$B/build" && "$src/configure" -q --target=$target --with-platform=efi \
    --disable-werror >/dev/null && make -s -j"$(nproc)" >/dev/null)

# BOOTX64.EFI or BOOTAA64.EFI: GRUB with gork, the network and the usual
# file systems in it
d=$B/pack
mkdir -p "$d/EFI/BOOT"
g=$B/build/grub-core
"$B/build/grub-mkimage" -d "$g" -O $fmt -p /EFI/BOOT \
    -o "$d/EFI/BOOT/$boot" \
    gork normal configfile echo cat ls test true sleep reboot halt \
    part_gpt part_msdos fat ext2 iso9660 search search_fs_file \
    efinet net linux chain efi_gop all_video
cp "$g/gork.mod" README.md LICENSE "$d/"
cp docs/build/grub.md "$d/BUILD.md"
z=$OUT/gork-$ver-grub-$ARCH.zip
rm -f "$z"
(cd "$d" && zip -qr "$z" .)
echo "$z"
