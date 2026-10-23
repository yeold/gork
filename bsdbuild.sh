#!/bin/sh
# Cross-build gork for FreeBSD, NetBSD and OpenBSD, amd64 and arm64, as
# static binaries, and pack each as OUT/gork-<version>-<os>-<arch>.tar.gz.
# Built on Linux with clang and lld against a sysroot unpacked from each
# release's own base sets (checksums pinned), so the binary uses the
# system's libc and curses, linked in: FreeBSD's ncursesw, NetBSD's
# curses and terminfo, OpenBSD's ncurses.  OpenBSD has no stable system
# call interface between releases, so its binaries are for that release.
# Needs clang, lld, curl and xz-utils; run from the source tree:
#     sh bsdbuild.sh [OUT] [WORK]
#     TARGETS="freebsd-amd64" sh bsdbuild.sh       # only some
set -eu
OUT=${1:-out}
WORK=${2:-/tmp/gork-bsd}
TARGETS=${TARGETS:-"freebsd-amd64 freebsd-arm64 netbsd-amd64 netbsd-arm64
    openbsd-amd64 openbsd-arm64"}

FB=https://download.freebsd.org/releases
NB=https://cdn.netbsd.org/pub/NetBSD/NetBSD-11.0
OB=https://cdn.openbsd.org/pub/OpenBSD/7.9

mkdir -p "$OUT" "$WORK/dl"
OUT=$(cd "$OUT" && pwd)
WORK=$(cd "$WORK" && pwd)

# fetch FILE URL SHA256
fetch() {
    f=$WORK/dl/$1
    [ -f "$f" ] || curl -fsSL -o "$f" "$2"
    echo "$3  $f" | sha256sum -c - >/dev/null
}

# sysroot NAME FILE...: the headers and libraries out of the sets
sysroot() {
    s=$WORK/sys/$1
    shift
    [ -d "$s/usr/include" ] && return 0
    mkdir -p "$s"
    for f in "$@"; do
        tar xf "$WORK/dl/$f" -C "$s" ./usr/include ./usr/lib ./lib 2>/dev/null ||
            [ -d "$s/usr/include" ]
    done
}

ver=$(sed -n 's/^AC_INIT(\[gork\], \[\(.*\)\])/\1/p' configure.ac)
gver=$ver
if [ -e .git ] && git rev-parse --verify HEAD >/dev/null 2>&1; then
    dirty=
    git diff HEAD --quiet 2>/dev/null || dirty=-dirty
    gver="v${ver%%[~-]*}-beta-$(git rev-list HEAD | wc -l | tr -d ' 	')$dirty"
fi
for t in $TARGETS; do
    extra= rt=
    case $t in
    freebsd-amd64)
        fetch freebsd-15.1-amd64-base.txz $FB/amd64/15.1-RELEASE/base.txz \
            3768988b151c20f965679062b065c63a977d6bbb9f47fd83695ec2c40790c18f
        sysroot $t freebsd-15.1-amd64-base.txz
        triple=x86_64-unknown-freebsd15.1 libs=-lncursesw ;;
    freebsd-arm64)
        fetch freebsd-15.1-arm64-base.txz $FB/arm64/aarch64/15.1-RELEASE/base.txz \
            5b7a46a0abfbe23a1d4454b5600e2efcfce16b705bc7e9c851c37470c035ef98
        sysroot $t freebsd-15.1-arm64-base.txz
        triple=aarch64-unknown-freebsd15.1 libs=-lncursesw ;;
    netbsd-amd64)
        fetch netbsd-11.0-amd64-base.tar.xz $NB/amd64/binary/sets/base.tar.xz \
            06bc97d2ea7cbb7fea55e8b5fff17e26b642ebe3ca6475202954658e229e303c
        fetch netbsd-11.0-amd64-comp.tar.xz $NB/amd64/binary/sets/comp.tar.xz \
            e14cf9f6ea080d90a148918a4f73a3696c473c60a435985f4316a9033abc0b3e
        sysroot $t netbsd-11.0-amd64-base.tar.xz netbsd-11.0-amd64-comp.tar.xz
        triple=x86_64-unknown-netbsd11.0 libs="-lcurses -lterminfo" ;;
    netbsd-arm64)
        fetch netbsd-11.0-arm64-base.tar.xz $NB/evbarm-aarch64/binary/sets/base.tar.xz \
            28759aadf02f2a597d0c9686f7cac70d4c48a4d6f6a4cd2e3839c13fc8fc19c8
        fetch netbsd-11.0-arm64-comp.tar.xz $NB/evbarm-aarch64/binary/sets/comp.tar.xz \
            43ff8ea12ea32cab362af8e0b298f8c83ce6efddc29888dc4f50249a5bc379bd
        sysroot $t netbsd-11.0-arm64-base.tar.xz netbsd-11.0-arm64-comp.tar.xz
        # libc's malloc wants the arm64 atomics helpers from libgcc
        triple=aarch64-unknown-netbsd11.0 libs="-lcurses -lterminfo" rt=-lgcc ;;
    openbsd-amd64)
        fetch openbsd-7.9-amd64-base79.tgz $OB/amd64/base79.tgz \
            923d2e03f06408d50d4848334398c6d04b5514dcac7917badfc178a0eef248de
        fetch openbsd-7.9-amd64-comp79.tgz $OB/amd64/comp79.tgz \
            21a67af20aebcabf85b09f4206fc95b4cae0a35d42b154b976f0159f457724f9
        sysroot $t openbsd-7.9-amd64-base79.tgz openbsd-7.9-amd64-comp79.tgz
        triple=x86_64-unknown-openbsd7.9 libs=-lcurses ;;
    openbsd-arm64)
        fetch openbsd-7.9-arm64-base79.tgz $OB/arm64/base79.tgz \
            9a4b16701ec11103847ca314527de5caf5f811c1dbc0e348d56ed8e9d9f16cb1
        fetch openbsd-7.9-arm64-comp79.tgz $OB/arm64/comp79.tgz \
            87d257a0e191a32fdebe9c2755e7245c81472f41456ea2fd1ca9832ed55fc417
        sysroot $t openbsd-7.9-arm64-base79.tgz openbsd-7.9-arm64-comp79.tgz
        # clang 19 crashes on main.c for this target with the stack
        # protector on
        triple=aarch64-unknown-openbsd7.9 libs=-lcurses extra=-fno-stack-protector ;;
    *)
        echo "bsdbuild.sh: unknown target $t" >&2
        exit 2 ;;
    esac
    case $t in
    # OpenBSD's static binaries are position independent (static PIE);
    # clang would hand lld OpenBSD ld's -nopie, so ask lld directly
    openbsd-*) link="-Wl,-pie -Wl,--no-dynamic-linker" ;;
    *) link= ;;
    esac
    b=$WORK/$t
    mkdir -p "$b"
    clang --target=$triple --sysroot="$WORK/sys/$t" -fuse-ld=lld -static \
        -std=gnu89 -O2 $extra $link -DGORK_UNICODE -DVERSION="\"$gver\"" -o "$b/gork" \
        main.c relay.c tui.c mcp.c cJSON/cJSON.c $libs -lm $rt \
        2>"$b/build.log" || { cat "$b/build.log"; exit 1; }
    llvm-strip "$b/gork" 2>/dev/null || strip "$b/gork" 2>/dev/null || true
    # the fake API for testing/bsdtest.py, not packed
    clang --target=$triple --sysroot="$WORK/sys/$t" -fuse-ld=lld -static \
        -O2 $extra $link -o "$b/fakeapi" testing/fakeapi.c $rt \
        2>>"$b/build.log" || { cat "$b/build.log"; exit 1; }
    d=$b/gork-$ver-$t
    rm -rf "$d" && mkdir "$d"
    cp "$b/gork" README.md docs/BUILD.md LICENSE "$d/"
    tar czf "$OUT/gork-$ver-$t.tar.gz" -C "$b" "gork-$ver-$t"
    echo "$OUT/gork-$ver-$t.tar.gz"
done
