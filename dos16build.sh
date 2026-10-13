#!/bin/sh
# Cross-build the 16-bit gork.exe for DOS (real mode, 8086 and up) and pack
# it as OUT/gork-<version>-dos16.zip.  Fetches the Open Watcom compiler and
# the sources of Watt-32 and PDCurses into WORK, checksums pinned, and
# builds both libraries for the large memory model.  Needs curl, xz, tar,
# zip and an x86-64 Linux; run from the source tree:
#     sh dos16build.sh [OUT] [WORK]
set -eu
OUT=${1:-out}
WORK=${2:-/tmp/gork-dos16}
OW=https://github.com/open-watcom/open-watcom-v2/releases/download/2026-10-01-Build/ow-snapshot.tar.xz
WT=https://github.com/gvanem/Watt-32/archive/c727114d209138e3057c122134cf7c3f3e67c374.tar.gz
PD=https://github.com/wmcbrine/PDCurses/archive/refs/tags/3.9.tar.gz

get() {     # get URL NAME SHA256: download into $WORK unless already there
    f=$WORK/$2
    [ -f "$f" ] || curl -fsSL -o "$f" "$1"
    echo "$3  $f" | sha256sum -c - >/dev/null
}

mkdir -p "$OUT" "$WORK/pack"
OUT=$(cd "$OUT" && pwd)
WORK=$(cd "$WORK" && pwd)
SRC=$(pwd)
get $OW ow-snapshot.tar.xz e6aa1b1e40ac8bbf97658d2c70fff8a4242d6ca4a1c60806f2baa5317083d4fe
get $WT watt-32.tar.gz fa02b4efd42160177a2208482877343b335a925eaded354352e5160fbd3f1022
get $PD PDCurses-3.9.tar.gz 590dbe0f5835f66992df096d3602d0271103f90cf8557a5d124f693c2b40d7ec

if [ ! -d "$WORK/ow" ]; then
    mkdir "$WORK/ow"
    tar xJf "$WORK/ow-snapshot.tar.xz" -C "$WORK/ow" ./binl64 ./h ./lib286
fi
WATCOM=$WORK/ow
PATH=$WATCOM/binl64:$PATH
INCLUDE=$WATCOM/h
export WATCOM PATH INCLUDE

# Watt-32 for the large model, with none of its optional parts
# (-DOPT_DEFINED): no BSD sockets, no DHCP, no debugging, which leaves
# room in 640K.  Its makefiles want a list of the compiler's error numbers
# that a DOS program of theirs prints; dos16/ has that output.
if [ ! -f "$WORK/watt/lib/wattcpwl.lib" ]; then
    rm -rf "$WORK/watt" && mkdir "$WORK/watt"
    tar xzf "$WORK/watt-32.tar.gz" -C "$WORK/watt" --strip-components=1
    mkdir -p "$WORK/watt/src/build/watcom"
    cp dos16/watcom.err "$WORK/watt/inc/sys/watcom.err"
    cp dos16/syserr.c "$WORK/watt/src/build/watcom/syserr.c"
    (cd "$WORK/watt/src"
     chmod +x ../util/linux/*
     sh configur.sh watcom >/dev/null 2>&1 || true     # its last step wants dosemu
     sed -i 's/-DWATT32_BUILD/-DWATT32_BUILD -DOPT_DEFINED/' watcom_l.mak
     wmake -h -f watcom_l.mak >"$WORK/watt.log" 2>&1) ||
        { tail -20 "$WORK/watt.log"; exit 1; }
fi

if [ ! -f "$WORK/pdcurses/dos/pdcurses.lib" ]; then
    rm -rf "$WORK/pdcurses" && mkdir "$WORK/pdcurses"
    tar xzf "$WORK/PDCurses-3.9.tar.gz" -C "$WORK/pdcurses" --strip-components=1
    (cd "$WORK/pdcurses/dos"
     for f in ../pdcurses/*.c *.c; do
         wcc -bt=dos -ml -zq -i=.. -oneatx "$f"
     done
     wlib -q -n -b -c -t pdcurses.lib ./*.o)
fi

# gork.  -zc, -zm and a code segment of its own for each file (-nt): the
# string constants and the code would not fit in one 64K segment, nor the
# constants with the data.  -zt64: larger data out of the 64K data
# segment.  -fpc: floating point by calls, since the 8087 emulator alone
# overfills the C library's code segment.
obj=$WORK/obj
rm -rf "$obj" && mkdir "$obj"
for f in main.c relay.c tui.c cJSON/cJSON.c; do
    n=$(basename "$f" .c)
    wcc -q -ml -bt=dos -os -fpc -w3 -wcd=200 -wcd=201 -zq -zc -zm -zt64 -nt="${n}_T" \
        -DCJSON_NESTING_LIMIT=40 -I"$WORK/pdcurses" -I"$WORK/watt/inc" \
        -fo="$obj/$n.obj" "$f"
done
# packcode=1: the linker must not gather code segments into groups, or
# Watt-32's interrupt code, which is written for a segment of its own,
# runs astray: sock_init() then crashes or not, depending on the layout.
(cd "$obj" && wlink option quiet system dos option packcode=1 \
    option packdata=1 option stack=24k option map=gork.map name gork.exe \
    file main.obj,relay.obj,tui.obj,cJSON.obj \
    library "$WORK/pdcurses/dos/pdcurses.lib","$WORK/watt/lib/wattcpwl.lib" 2>&1 |
    grep -v W1027 || true)
test -s "$obj/gork.exe"

ver=$(sed -n 's/^AC_INIT(\[gork\], \[\(.*\)\])/\1/p' configure.ac)
cp "$obj/gork.exe" README.md docs/BUILD.md LICENSE "$WORK/pack/"
rm -f "$OUT/gork-$ver-dos16.zip"
(cd "$WORK/pack" && zip -q "$OUT/gork-$ver-dos16.zip" ./*)
echo "$OUT/gork-$ver-dos16.zip"
