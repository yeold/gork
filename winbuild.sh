#!/bin/sh
# Cross-build gork.exe for Windows (9x and NT, 32-bit) with MinGW and pack
# it as OUT/gork-<version>-win32-legacy.zip ("legacy": the Cygwin build,
# cygpack.sh, is the Windows package for current systems, and has MCP).
# Fetches the PDCurses source into
# WORK, checksum pinned.  Needs the i686 mingw-w64 gcc, curl, zip and make;
# run from the source tree:  sh winbuild.sh [OUT] [WORK]
set -eu
OUT=${1:-out}
WORK=${2:-/tmp/gork-win}
HOST=${HOST:-i686-w64-mingw32}
PD=https://github.com/wmcbrine/PDCurses/archive/refs/tags/3.9.tar.gz

mkdir -p "$OUT" "$WORK/pack"
OUT=$(cd "$OUT" && pwd)
f=$WORK/PDCurses-3.9.tar.gz
[ -f "$f" ] || curl -fsSL -o "$f" "$PD"
echo "590dbe0f5835f66992df096d3602d0271103f90cf8557a5d124f693c2b40d7ec  $f" | sha256sum -c - >/dev/null

[ -d "$WORK/PDCurses-3.9" ] || tar xzf "$f" -C "$WORK"
make -s -C "$WORK/PDCurses-3.9/wincon" \
    CC="$HOST-gcc -march=i486 -mtune=generic -D__USE_MINGW_ANSI_STDIO=0" \
    AR="$HOST-ar" \
    STRIP="$HOST-strip" pdcurses.a

make -f Makefile.w32 CC="$HOST-gcc" PDCURSES="$WORK/PDCurses-3.9"
"$HOST-strip" gork.exe

ver=$(sed -n 's/^AC_INIT(\[gork\], \[\(.*\)\])/\1/p' configure.ac)
cp gork.exe README.md docs/BUILD.md "$WORK/pack/"
rm -f "$OUT/gork-$ver-win32-legacy.zip"
(cd "$WORK/pack" && zip -q "$OUT/gork-$ver-win32-legacy.zip" *)
echo "$OUT/gork-$ver-win32-legacy.zip"
