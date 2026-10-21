// Source tarball, then RPMs and debs built from it; static Linux binaries
// for ten CPUs (linuxbuild.sh), each run through the suite under qemu-user;
// the DOS zips, 32-bit (dosbuild.sh) and 16-bit (dos16build.sh), tested on
// FreeDOS in QEMU (testing/dostest.py); and the legacy Windows zip, cross-
// built with MinGW (winbuild.sh) and tested under Wine (testing/wintest.py);
// static FreeBSD, NetBSD and OpenBSD binaries cross-built with clang
// (bsdbuild.sh), each run on its own system in QEMU (testing/bsdtest.py);
// and gork for x86-64 and arm64 UEFI (uefibuild.sh), run in the UEFI
// shell under OVMF, AAVMF and U-Boot (testing/uefitest.py).  Each in a throwaway
// container via plain `docker run` (no Docker Pipeline plugin needed).
// Containers build as root in their own /tmp and only copy finished
// packages into out/, chowned back to the Jenkins user.
pipeline {
    agent { label 'docker' }
    options { buildDiscarder(logRotator(numToKeepStr: '20')) }
    stages {
        stage('dist') {
            steps {
                sh '''
                    # bsdwork/ was the bsd stage's cache in the workspace
                    # once; every stage copies the workspace, so not there
                    rm -rf out bsdwork && mkdir out
                    docker run --rm -v "$PWD":/src:ro -v "$PWD/out":/out debian:stable sh -ec '
                        apt-get -qq update
                        DEBIAN_FRONTEND=noninteractive apt-get -qq install -y build-essential autoconf automake libncurses-dev >/dev/null
                        cp -r /src /tmp/src && cd /tmp/src
                        autoreconf -i && ./configure && make dist
                        cp gork-*.tar.gz /out/ && chown -R '"$(id -u):$(id -g)"' /out'
                '''
            }
        }
        stage('packages') {
            parallel {
                stage('rpm') {
                    steps {
                        sh '''
                            docker run --rm -v "$PWD/out":/out fedora:latest sh -ec '
                                dnf -yq install rpm-build gcc make ncurses-devel
                                # the source tarball, not the Linux and BSD binaries beside it
                                rpmbuild -ta "$(ls /out/gork-*.tar.gz | grep -vE -- "-(linux|freebsd|netbsd|openbsd)-")" --define "_topdir /tmp/rpm"
                                cp /tmp/rpm/SRPMS/*.rpm /tmp/rpm/RPMS/*/*.rpm /out/ && chown -R '"$(id -u):$(id -g)"' /out'
                        '''
                    }
                }
                stage('linux') {
                    steps {
                        sh '''
                            docker run --rm -v "$PWD":/src:ro -v "$PWD/out":/out debian:stable sh -ec '
                                apt-get -qq update
                                DEBIAN_FRONTEND=noninteractive apt-get -qq install -y --no-install-recommends \
                                    gcc libc6-dev make curl ca-certificates ncurses-bin \
                                    gcc-x86-64-linux-gnu gcc-i686-linux-gnu gcc-aarch64-linux-gnu \
                                    gcc-arm-linux-gnueabihf gcc-riscv64-linux-gnu gcc-s390x-linux-gnu \
                                    gcc-powerpc64le-linux-gnu gcc-powerpc-linux-gnu gcc-m68k-linux-gnu \
                                    gcc-hppa-linux-gnu libc6-dev-i386-cross libc6-dev-arm64-cross \
                                    libc6-dev-armhf-cross libc6-dev-riscv64-cross libc6-dev-s390x-cross \
                                    libc6-dev-ppc64el-cross libc6-dev-powerpc-cross libc6-dev-m68k-cross \
                                    libc6-dev-hppa-cross >/dev/null
                                cp -r /src /tmp/src && cd /tmp/src
                                sh linuxbuild.sh /out && chown -R '"$(id -u):$(id -g)"' /out'
                            # and run each: the whole suite, under qemu-user
                            docker run --rm -v "$PWD":/src:ro -v "$PWD/out":/out:ro debian:stable sh -ec '
                                apt-get -qq update
                                DEBIAN_FRONTEND=noninteractive apt-get -qq install -y --no-install-recommends python3 qemu-user-static >/dev/null
                                cd /src && sh testing/linuxtest.sh /out'
                        '''
                    }
                }
                stage('dos') {
                    steps {
                        sh '''
                            docker run --rm -v "$PWD":/src:ro -v "$PWD/out":/out debian:stable sh -ec '
                                apt-get -qq update
                                DEBIAN_FRONTEND=noninteractive apt-get -qq install -y curl ca-certificates bzip2 unzip zip make >/dev/null
                                cp -r /src /tmp/src && cd /tmp/src
                                sh dosbuild.sh /out && chown -R '"$(id -u):$(id -g)"' /out'
                            # and run it: FreeDOS in QEMU against a fake API, with
                            # a gork-relay for the SPX check
                            docker run --rm -v "$PWD":/src:ro -v "$PWD/out":/out:ro debian:stable sh -ec '
                                apt-get -qq update
                                DEBIAN_FRONTEND=noninteractive apt-get -qq install -y --no-install-recommends qemu-system-x86 mtools unzip python3 ca-certificates curl gcc libc6-dev make libssl-dev >/dev/null
                                cd /src && relay=$(sh testing/getrelay.sh) && GORK_RELAY=$relay GORK_VM=/tmp/vm python3 testing/dostest.py /out/gork-*-dos.zip'
                        '''
                    }
                }
                stage('dos16') {
                    steps {
                        sh '''
                            docker run --rm -v "$PWD":/src:ro -v "$PWD/out":/out debian:stable sh -ec '
                                apt-get -qq update
                                DEBIAN_FRONTEND=noninteractive apt-get -qq install -y curl ca-certificates xz-utils zip >/dev/null
                                cp -r /src /tmp/src && cd /tmp/src
                                sh dos16build.sh /out && chown -R '"$(id -u):$(id -g)"' /out'
                            # the same checks as for the 32-bit one, with the
                            # address from DHCP here; then on an 8086 and a 286
                            # in DOSBox-X
                            docker run --rm -v "$PWD":/src:ro -v "$PWD/out":/out:ro debian:stable sh -ec '
                                apt-get -qq update
                                DEBIAN_FRONTEND=noninteractive apt-get -qq install -y --no-install-recommends qemu-system-x86 mtools unzip python3 ca-certificates curl gcc libc6-dev make libssl-dev dosbox-x >/dev/null
                                cd /src && relay=$(sh testing/getrelay.sh) && GORK_RELAY=$relay GORK_DOS_DHCP=1 GORK_VM=/tmp/vm python3 testing/dostest.py /out/gork-*-dos16.zip &&
                                    GORK_VM=/tmp/vm python3 testing/dosboxtest.py /out/gork-*-dos16.zip'
                        '''
                    }
                }
                stage('windows') {
                    steps {
                        sh '''
                            docker run --rm -v "$PWD":/src:ro -v "$PWD/out":/out debian:stable sh -ec '
                                apt-get -qq update
                                DEBIAN_FRONTEND=noninteractive apt-get -qq install -y --no-install-recommends gcc-mingw-w64-i686 curl ca-certificates zip make >/dev/null
                                cp -r /src /tmp/src && cd /tmp/src
                                sh winbuild.sh /out && chown -R '"$(id -u):$(id -g)"' /out'
                            # and run it: one gork.exe for 9x and NT, here under Wine
                            docker run --rm -v "$PWD":/src:ro -v "$PWD/out":/out:ro debian:stable sh -ec '
                                dpkg --add-architecture i386
                                apt-get -qq update
                                DEBIAN_FRONTEND=noninteractive apt-get -qq install -y --no-install-recommends wine wine32:i386 unzip python3 >/dev/null
                                mkdir /tmp/w && cd /tmp/w && unzip -q /out/gork-*-win32-legacy.zip
                                cd /src && python3 testing/wintest.py wine /tmp/w/gork.exe &&
                                    python3 testing/wintest.py wine /tmp/w/gorknt3.exe'
                        '''
                    }
                }
                // The BSD packages, and the UEFI programs in the same
                // containers: they want the same clang and QEMU, and a stage
                // of their own beside the others filled the agent's disk
                stage('bsd') {
                    steps {
                        // the release sets, the sysroots and the VM images,
                        // kept between builds (about 15 GB), outside the
                        // workspace: every stage copies that
                        sh '''
                            BW="$HOME/.cache/gork-bsd" && mkdir -p "$BW"
                            docker run --rm -v "$PWD":/src:ro -v "$PWD/out":/out -v "$BW":/work debian:stable sh -ec '
                                apt-get -qq update
                                DEBIAN_FRONTEND=noninteractive apt-get -qq install -y --no-install-recommends clang lld llvm curl ca-certificates xz-utils zip >/dev/null
                                cp -r /src /tmp/src && cd /tmp/src
                                sh bsdbuild.sh /out /work && sh uefibuild.sh /out /tmp/uefi &&
                                    chown -R '"$(id -u):$(id -g)"' /out /work'
                            # and run each on its own system: the official VM
                            # image, booted in QEMU, the checks of testing/vmcheck.sh
                            docker run --rm -v "$PWD":/src:ro -v "$PWD/out":/out:ro -v "$BW":/work debian:stable sh -ec '
                                apt-get -qq update
                                DEBIAN_FRONTEND=noninteractive apt-get -qq install -y --no-install-recommends qemu-system-x86 qemu-system-arm qemu-utils qemu-efi-aarch64 ovmf genisoimage python3 xz-utils gzip curl ca-certificates \
                                    u-boot-qemu efi-shell-x64 efi-shell-aa64 mtools dosfstools gcc libc6-dev >/dev/null
                                cd /src && mkdir -p /work/tmp && rc=0
                                for t in freebsd-amd64 freebsd-arm64 netbsd-amd64 netbsd-arm64 openbsd-amd64 openbsd-arm64; do
                                    echo "== $t"
                                    GORK_VM=/work/vm TMPDIR=/work/tmp python3 testing/bsdtest.py $t /out/gork-*-$t.tar.gz /work/$t/fakeapi || rc=1
                                done
                                # gork for UEFI from the UEFI shell: on the TCP/IP of the
                                # firmware (OVMF, AAVMF) and on its own (OVMF without its
                                # network stack, U-Boot), against a fake API built here
                                # (no apostrophes in here: this is all one quoted script)
                                gcc -o /tmp/fakeapi testing/fakeapi.c
                                for m in x64 aa64 x64-own uboot; do
                                    echo "== uefi $m"
                                    TMPDIR=/work/tmp python3 testing/uefitest.py /out/gork-*-uefi.zip /tmp/fakeapi $m || rc=1
                                done
                                chown -R '"$(id -u):$(id -g)"' /work
                                exit $rc'
                        '''
                    }
                }
                stage('deb') {
                    steps {
                        sh '''
                            docker run --rm -v "$PWD/out":/out debian:stable sh -ec '
                                apt-get -qq update
                                DEBIAN_FRONTEND=noninteractive apt-get -qq install -y build-essential debhelper libncurses-dev >/dev/null
                                mkdir /tmp/deb && tar xzf "$(ls /out/gork-*.tar.gz | grep -vE -- "-(linux|freebsd|netbsd|openbsd)-")" -C /tmp/deb
                                cd /tmp/deb/gork-* && dpkg-buildpackage -us -uc
                                cd /tmp/deb && cp *.deb *.dsc *.tar.xz *.buildinfo *.changes /out/ && chown -R '"$(id -u):$(id -g)"' /out'
                        '''
                    }
                }
            }
        }
        stage('release') {
            when { tag 'v*' }
            steps {
                withCredentials([string(credentialsId: 'github-release-token', variable: 'GITHUB_TOKEN'),
                                 string(credentialsId: 'forgejo-release-token', variable: 'FORGEJO_TOKEN')]) {
                    sh '''
                        docker run --rm -v "$PWD":/w:ro -w /w -e TAG="$TAG_NAME" -e GITHUB_TOKEN -e FORGEJO_TOKEN debian:stable sh -ec '
                            apt-get -qq update
                            DEBIAN_FRONTEND=noninteractive apt-get -qq install -y curl jq ca-certificates >/dev/null
                            sh release.sh out'
                    '''
                }
            }
        }
    }
    post {
        success { archiveArtifacts artifacts: 'out/*', fingerprint: true }
    }
}
