#!/bin/sh
# Fetch and build gork-relay for dostest.py's SPX check; prints the path of
# the binary, for $GORK_RELAY.  A release by its tag, checksum pinned.
# Needs curl, a C compiler, make and the OpenSSL headers:
#     GORK_RELAY=$(sh testing/getrelay.sh [DIR])
set -eu
DIR=${1:-/tmp/gork-relay}
V=0.4
mkdir -p "$DIR"
f=$DIR/gork-relay-$V.tar.gz
[ -f "$f" ] || curl -fsSL -o "$f" "https://github.com/yeold/gork-relay/archive/refs/tags/v$V.tar.gz"
echo "6941d47d48ca15b89838eb0f218c59a1c1374473f58dc069391d769313c26847  $f" | sha256sum -c - >/dev/null
[ -d "$DIR/gork-relay-$V" ] || tar xzf "$f" -C "$DIR"
make -s -C "$DIR/gork-relay-$V" gork-relay >&2
echo "$DIR/gork-relay-$V/gork-relay"
