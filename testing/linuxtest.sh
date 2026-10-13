#!/bin/sh
# Run testing/looptest.py on every gork-*-linux-<arch>.tar.gz in OUT, each
# under QEMU's user-mode emulation (qemu-user-static), as linuxbuild.sh
# made them:  sh testing/linuxtest.sh [OUT]
set -eu
OUT=${1:-out}
T=$(mktemp -d)
rc=0
for f in "$OUT"/gork-*-linux-*.tar.gz; do
    arch=${f##*-linux-}
    arch=${arch%.tar.gz}
    case $arch in
        amd64)   q= ;;
        arm64)   q=qemu-aarch64-static ;;
        armhf)   q=qemu-arm-static ;;
        ppc64el) q=qemu-ppc64le-static ;;
        powerpc) q=qemu-ppc-static ;;
        *)       q=qemu-$arch-static ;;
    esac
    tar xzf "$f" -C "$T"
    bin=$T/$(basename "$f" .tar.gz)/gork
    # looptest.py runs one program: give it the emulator and gork as one
    printf '#!/bin/sh\nexec %s %s "$@"\n' "$q" "$bin" > "$T/gork-$arch"
    chmod +x "$T/gork-$arch"
    if python3 testing/looptest.py "$T/gork-$arch" > "$T/$arch.log" 2>&1; then
        echo "$arch: $(tail -n 1 "$T/$arch.log")"
    else
        echo "$arch: FAILED"
        tail -n 25 "$T/$arch.log"
        rc=1
    fi
done
rm -rf "$T"
exit $rc
