#!/bin/sh
# Fetch and build gork-relay for dostest.py's SPX check; prints the path of
# the binary, for $GORK_RELAY.  A release by its tag, checksum pinned.
# Needs curl, a C compiler, make and the OpenSSL headers:
#     GORK_RELAY=$(sh testing/getrelay.sh [DIR])
set -eu
DIR=${1:-/tmp/gork-relay}
V=0.1
mkdir -p "$DIR"
f=$DIR/gork-relay-$V.tar.gz
[ -f "$f" ] || curl -fsSL -o "$f" "https://github.com/yeold/gork-relay/archive/refs/tags/v$V.tar.gz"
echo "578a71ea02daf6fb1faf665b38305767cd83167208cfd7751fed433d444a2317  $f" | sha256sum -c - >/dev/null
[ -d "$DIR/gork-relay-$V" ] || tar xzf "$f" -C "$DIR"
make -s -C "$DIR/gork-relay-$V" gork-relay >&2
echo "$DIR/gork-relay-$V/gork-relay"
