// Source tarball, then RPMs and debs built from it, each in a throwaway
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
    }
    post {
        success { archiveArtifacts artifacts: 'out/*', fingerprint: true }
    }
}
