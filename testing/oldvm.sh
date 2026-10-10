#!/bin/sh
# Debian 1.3 "bo" (1997) in QEMU: Linux 2.0.33, libc 5.4.33, gcc 2.7.2.1,
# ncurses 1.9.9e -- the oldest userland gork is tried on, with its own
# kernel (libc5 cannot run under a modern one, so Docker is no use here).
#
#   testing/oldvm.sh build   make the disk image (needs docker, curl)
#   testing/oldvm.sh run     boot it; serial console on this terminal
#
# `run` hands the guest a fresh `make dist` tarball as /dev/hdb.  Log in as
# root (no password) and run `./build.sh` to unpack, configure and make it
# in /root/gork-<version>.  The host is 10.0.2.2 from inside, so a fake API
# or relay listening on the host works as `relay = 10.0.2.2:<port>`.
# The kernel has no serial console: boot is silent for a few seconds, then
# a login prompt appears.  `halt` in the guest, then Ctrl-A X, to leave.
#
# The image lives in $GORK_VM (default ~/.cache/gork-vm).
set -e

VM=${GORK_VM:-$HOME/.cache/gork-vm}
MIRROR=http://archive.debian.org/debian/dists/bo/main/binary-i386
DEBS="base/kernel-image-2.0.33_2.0.33-3 devel/binutils_2.7.0.9-3
      libs/libbfd2.7.0.9_2.7.0.9-3
      interpreters/cpp_2.7.2.1-9 devel/gcc_2.7.2.1-9 devel/libc5-dev_5.4.33-6
      devel/make_3.75-4 devel/ncurses3.0-dev_1.9.9e-1
      admin/ncurses-term_1.9.9e-1 utils/strace_3.1-8"

build() {
    mkdir -p "$VM/debs"
    for d in $DEBS; do
        f=$VM/debs/${d#*/}.deb
        [ -f "$f" ] || curl -sfo "$f" "$MIRROR/$d.deb"
    done
    # Root in a throwaway container: device nodes and file owners survive,
    # and mke2fs -d fills the image without mounting anything.
    c=$(docker create debian/eol:bo)
    trap 'docker rm "$c" >/dev/null' EXIT
    docker export "$c" | docker run --rm -i -v "$VM":/vm debian:stable sh -ec '
        apt-get -qq update </dev/null
        DEBIAN_FRONTEND=noninteractive apt-get -qq install -y e2fsprogs </dev/null >/dev/null
        mkdir /r && tar x -C /r
        for d in /vm/debs/*.deb; do dpkg-deb -x "$d" /r; done
        # qemu -kernel hangs on a 2.0 kernel, but one written raw to a
        # floppy boots itself; bytes 508-509 are its root device (/dev/hda).
        cp /r/boot/vmlinuz-2.0.33 /vm/boot.img
        printf "\\000\\003" | dd of=/vm/boot.img bs=1 seek=508 conv=notrunc 2>/dev/null
        truncate -s 1440k /vm/boot.img
        cd /r
        rm -f .dockerenv
        echo gorkvm > etc/hostname
        printf "/dev/hda / ext2 defaults 0 1\nproc /proc proc defaults 0 0\n" > etc/fstab
        echo serial > etc/modules
        echo "T0:23:respawn:/sbin/getty 9600 ttyS0" >> etc/inittab
        echo ttyS0 >> etc/securetty
        # the debs were only unpacked, so nothing has registered their libs
        printf "#!/bin/sh\n/sbin/ldconfig\n" > etc/rc.boot/ldconfig
        cat > etc/init.d/network <<EOF
#!/bin/sh
# here, not in /etc/modules: only its first line ever gets loaded
modprobe ne io=0x300 irq=10
ifconfig lo 127.0.0.1
route add -net 127.0.0.0
ifconfig eth0 10.0.2.15 netmask 255.255.255.0 broadcast 10.0.2.255
route add -net 10.0.2.0 netmask 255.255.255.0 eth0
route add default gw 10.0.2.2 metric 1
EOF
        cat > root/.bash_profile <<EOF
PATH=/usr/local/sbin:/usr/sbin:/sbin:\$PATH; export PATH
case \`tty\` in /dev/ttyS*) TERM=vt100; export TERM; stty rows 24 cols 80;; esac
EOF
        cat > root/build.sh <<EOF
#!/bin/sh
# unpack the tarball the host put on /dev/hdb, then build it
set -e
cd /root && rm -rf gork-* && tar xf /dev/hdb && tar xzf gork-*.tar.gz
cd gork-*/ && ./configure && make
EOF
        chmod 755 etc/init.d/network etc/rc.boot/ldconfig root/build.sh
        rm -f /vm/disk.img
        # revision 0: the original ext2 layout, all a 2.0 kernel and its fsck know
        mke2fs -q -t ext2 -E revision=0 -d /r /vm/disk.img 400M
        chown '"$(id -u):$(id -g)"' /vm/boot.img /vm/disk.img'
    echo "built $VM/disk.img"
}

run() {
    [ -f "$VM/disk.img" ] || { echo "no image; run: $0 build" >&2; exit 1; }
    top=$(cd "$(dirname "$0")/.." && pwd)
    make -s -C "$top" dist
    tar --format=ustar -cf "$VM/src.tar" -C "$top" "$(cd "$top" && ls -t gork-*.tar.gz | head -1)"
    exec qemu-system-i386 -m 64 -display none -serial mon:stdio \
        -drive file="$VM/boot.img",format=raw,if=floppy -boot a \
        -drive file="$VM/disk.img",format=raw,index=0,media=disk \
        -drive file="$VM/src.tar",format=raw,index=1,media=disk \
        -netdev user,id=n0 -device ne2k_isa,netdev=n0,iobase=0x300,irq=10
}

case $1 in
build|run) "$1" ;;
*) echo "usage: $0 build|run" >&2; exit 2 ;;
esac
