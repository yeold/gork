#!/bin/sh
# Cross-build gork for Linux on every CPU it has passed its suite on, as
# static binaries, and pack each as OUT/gork-<version>-linux-<arch>.tar.gz.
# Static, with ncurses built in: one file that runs on any Linux of that
# CPU, whatever its libc and curses.  Fetches the ncurses source into
# WORK, checksum pinned.  Needs Debian's cross compilers (gcc-<triplet>
# for each line of ARCHES, and gcc for the build machine), make, curl and
# ncurses-bin (tic); run from the source tree:
#     sh linuxbuild.sh [OUT] [WORK]
#     ARCHES="arm64:aarch64-linux-gnu" sh linuxbuild.sh      # only some
set -eu
OUT=${1:-out}
WORK=${2:-/tmp/gork-linux}
NC=https://ftp.gnu.org/gnu/ncurses/ncurses-6.5.tar.gz
ARCHES=${ARCHES:-"amd64:x86_64-linux-gnu i386:i686-linux-gnu
    arm64:aarch64-linux-gnu armhf:arm-linux-gnueabihf
    riscv64:riscv64-linux-gnu s390x:s390x-linux-gnu
    ppc64el:powerpc64le-linux-gnu powerpc:powerpc-linux-gnu
    m68k:m68k-linux-gnu hppa:hppa-linux-gnu"}

mkdir -p "$OUT" "$WORK"
OUT=$(cd "$OUT" && pwd)
WORK=$(cd "$WORK" && pwd)
f=$WORK/ncurses-6.5.tar.gz
[ -f "$f" ] || curl -fsSL -o "$f" "$NC"
echo "136d91bc269a9a5785e5f9e980bc76ab57428f604ce3e5a5a90cebc767971cc6  $f" | sha256sum -c - >/dev/null
[ -d "$WORK/ncurses-6.5" ] || tar xzf "$f" -C "$WORK"

ver=$(sed -n 's/^AC_INIT(\[gork\], \[\(.*\)\])/\1/p' configure.ac)
gver=$(sh version.sh)          # what --version says
for a in $ARCHES; do
    arch=${a%%:*}
    host=${a#*:}
    b=$WORK/$arch
    # The wide library, for UTF-8 terminals.  It reads the system's
    # terminal descriptions where there are any, and has the common ones
    # built in for where there are none.
    if [ ! -f "$b/nc/lib/libncursesw.a" ]; then
        rm -rf "$b" && mkdir -p "$b/build"
        (cd "$b/build" && "$WORK/ncurses-6.5/configure" --host="$host" \
            --prefix="$b/nc" --enable-widec --without-shared --without-debug \
            --without-cxx --without-cxx-binding --without-ada --without-progs \
            --without-tests --without-manpages --disable-stripping \
            --with-default-terminfo-dir=/usr/share/terminfo \
            --with-terminfo-dirs=/etc/terminfo:/lib/terminfo:/usr/share/terminfo \
            --with-fallbacks=linux,xterm,xterm-256color,vt100,vt220,screen,tmux,ansi \
            >"$b/ncurses.log" 2>&1 &&
         make -j"$(nproc)" libs >>"$b/ncurses.log" 2>&1 &&
         make install.libs install.includes >>"$b/ncurses.log" 2>&1) ||
            { tail -20 "$b/ncurses.log"; exit 1; }
    fi
    # _FILE_OFFSET_BITS: on a 32-bit CPU, a directory on a file system
    # with 64-bit inode numbers (XFS, btrfs, overlayfs) otherwise reads as
    # empty, and stat() fails there.
    "$host-gcc" -std=gnu89 -O2 -static -D_FILE_OFFSET_BITS=64 \
        -DGORK_UNICODE -DVERSION="\"$gver\"" \
        -I"$b/nc/include/ncursesw" -I"$b/nc/include" -o "$b/gork" \
        main.c relay.c tui.c mcp.c cJSON/cJSON.c \
        -L"$b/nc/lib" -lncursesw -lm 2>"$b/link.log" ||
        { cat "$b/link.log"; exit 1; }
    "$host-strip" "$b/gork"
    d=$b/gork-$ver-linux-$arch
    rm -rf "$d" && mkdir "$d"
    cp "$b/gork" README.md docs/BUILD.md LICENSE "$d/"
    tar czf "$OUT/gork-$ver-linux-$arch.tar.gz" -C "$b" "gork-$ver-linux-$arch"
    echo "$OUT/gork-$ver-linux-$arch.tar.gz"
done
