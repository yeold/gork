#!/bin/sh
# Cross-build gork.exe for DOS and pack it with CWSDPMI as
# OUT/gork-<version>-dos.zip.  Fetches a prebuilt DJGPP cross compiler,
# PDCurses and Watt-32 into WORK, checksums pinned.  Needs curl, bzip2,
# unzip, zip and make; run from the source tree:  sh dosbuild.sh [OUT] [WORK]
set -eu
OUT=${1:-out}
WORK=${2:-/tmp/gork-dos}
DJ=https://github.com/andrewwutw/build-djgpp/releases/download/v3.4/djgpp-linux64-gcc1220.tar.bz2
TK=https://www.delorie.com/pub/djgpp/current

get() {     # get URL SHA256: download into $WORK unless already there
    f=$WORK/$(basename "$1")
    [ -f "$f" ] || curl -fsSL -o "$f" "$1"
    echo "$2  $f" | sha256sum -c - >/dev/null
}

mkdir -p "$OUT" "$WORK"
OUT=$(cd "$OUT" && pwd)
get $DJ 8464f17017d6ab1b2bb2df4ed82357b5bf692e6e2b7fee37e315638f3d505f00
get $TK/v2tk/pdcur39a.zip e6522b089367343287959884b27a2c7ffd5a987cc6c2b40cfae030d5de3260ff
get $TK/v2tk/wat3211b.zip faa2222ab5deb2c2aac229c760bf4d45aca5379f5af97865c308a0467046b67a
get $TK/v2misc/csdpmi7b.zip deacda0488e1cdd7c4a9f32fab45662b34c0ed6b2d7d4d13bc07041b62004a8c

[ -d "$WORK/djgpp" ] || tar xjf "$WORK/djgpp-linux64-gcc1220.tar.bz2" -C "$WORK"
unzip -qo "$WORK/pdcur39a.zip" -d "$WORK/pdcurses"
unzip -qo "$WORK/wat3211b.zip" -d "$WORK/watt"
unzip -qoj "$WORK/csdpmi7b.zip" bin/CWSDPMI.EXE bin/cwsdpmi.doc -d "$WORK/pack"

make -f Makefile.dj CC="$WORK/djgpp/bin/i586-pc-msdosdjgpp-gcc" \
    PDCURSES="$WORK/pdcurses" WATT="$WORK/watt/net/watt"
"$WORK/djgpp/bin/i586-pc-msdosdjgpp-strip" gork.exe

ver=$(sed -n 's/^AC_INIT(\[gork\], \[\(.*\)\])/\1/p' configure.ac)
cp gork.exe README.md docs/BUILD.md "$WORK/pack/"
rm -f "$OUT/gork-$ver-dos.zip"
(cd "$WORK/pack" && zip -q "$OUT/gork-$ver-dos.zip" *)
echo "$OUT/gork-$ver-dos.zip"
