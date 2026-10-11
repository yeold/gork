// Source tarball, then RPMs and debs built from it and a DOS zip
// cross-built from the tree (dosbuild.sh) and tested on FreeDOS in QEMU
// (testing/dostest.py), each in a throwaway
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
                    rm -rf out && mkdir out
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
                                rpmbuild -ta /out/gork-*.tar.gz --define "_topdir /tmp/rpm"
                                cp /tmp/rpm/SRPMS/*.rpm /tmp/rpm/RPMS/*/*.rpm /out/ && chown -R '"$(id -u):$(id -g)"' /out'
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
                            # and run it: FreeDOS in QEMU against a fake API
                            docker run --rm -v "$PWD":/src:ro -v "$PWD/out":/out:ro debian:stable sh -ec '
                                apt-get -qq update
                                DEBIAN_FRONTEND=noninteractive apt-get -qq install -y --no-install-recommends qemu-system-x86 mtools unzip python3 ca-certificates >/dev/null
                                cd /src && GORK_VM=/tmp/vm python3 testing/dostest.py /out/gork-*-dos.zip'
                        '''
                    }
                }
                stage('deb') {
                    steps {
                        sh '''
                            docker run --rm -v "$PWD/out":/out debian:stable sh -ec '
                                apt-get -qq update
                                DEBIAN_FRONTEND=noninteractive apt-get -qq install -y build-essential debhelper libncurses-dev >/dev/null
                                mkdir /tmp/deb && tar xzf /out/gork-*.tar.gz -C /tmp/deb
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
