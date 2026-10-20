#!/bin/sh
# vmcheck.sh GORK FAKEAPI [MCPSH]: the batch checks for a system where
# gork runs but Python and the host may not, typically inside a VM.
# GORK is the binary under test, FAKEAPI testing/fakeapi.c built for the
# same system, MCPSH testing/mcp.sh (optional: the MCP check is skipped
# without it).  Plain Bourne shell and the base system's tools only.
# Works in a scratch directory under /tmp, with HOME pointed there, so the
# user's own config is not touched.  Prints "ok:" or "FAIL:" per check and
# exits 1 if any failed.
G=$1 F=$2 M=${3:-}
[ -x "$G" ] && [ -x "$F" ] || { echo "usage: vmcheck.sh GORK FAKEAPI [MCPSH]" >&2; exit 2; }
case $G in /*) ;; *) G=`pwd`/$G ;; esac
case $F in /*) ;; *) F=`pwd`/$F ;; esac
case $M in /*|'') ;; *) M=`pwd`/$M ;; esac
T=/tmp/vmcheck.$$
PORT=${PORT:-8099}
rm -rf $T; mkdir $T $T/home $T/home/.gork $T/f $T/w $T/w/sub || exit 2
HOME=$T/home; export HOME
fails=0
ok()   { echo "ok: $1"; }
fail() { echo "FAIL: $1"; fails=`expr $fails + 1`; }
# has STRING FILE: the fake's log holds lines longer than some grep take
has()  { awk -v s="$1" 'index($0, s) { f = 1 } END { exit !f }' "$2"; }

(cd $T/f && exec "$F" $PORT) >$T/fake.out 2>&1 &
fpid=$!
sleep 2
cat >$HOME/.gork.conf <<EOF
relay = 127.0.0.1:$PORT
api_key = sk-fake
timeout = 20
retries = 0
EOF
chmod 600 $HOME/.gork.conf
if [ -n "$M" ]; then
    echo '{"mcpServers": {"sh": {"command": "/bin/sh", "args": ["'$M'"]}}}' >$HOME/.gork/mcp.json
fi
cd $T/w
echo old >abcdefghijkl.c
ln -s sub lnk 2>/dev/null
LOG=$T/f/fakeapi.log

if "$G" --version | grep -q gork; then ok "--version"; else fail "--version"; fi
out=`"$G" hello 2>&1`
case $out in *PONG*) ok "a reply" ;; *) fail "a reply: $out" ;; esac

GORK_ALLOW_RUN=1 "$G" go >$T/go.out 2>&1
if [ "`cat out.txt 2>/dev/null`" = "a
b" ]; then ok "write_file"; else fail "write_file"; fi
if has 'abcdefghijkl.c' $LOG; then ok "list_dir"; else fail "list_dir"; fi
if has '[exit 0]' $LOG; then ok "run_command"; else fail "run_command"; fi

"$G" esc >/dev/null 2>&1
if [ ! -f $T/escape.txt ] && has 'outside the working directory' $LOG; then
    ok "paths outside the working directory refused"
else
    fail "paths outside the working directory refused"
fi

s=`date +%s 2>/dev/null`
GORK_ALLOW_RUN=1 "$G" t1 >/dev/null 2>&1
e=`date +%s 2>/dev/null`
if has 'timed out after 20 s, killed' $LOG; then ok "run_command timeout"; else fail "run_command timeout"; fi
if ps -ax 2>/dev/null | grep -v grep | grep -q 'sleep 100'; then
    fail "the timed-out command is gone"
else
    ok "the timed-out command is gone"
fi
case $s$e in *[!0-9]*|'') ;; *)
    [ `expr $e - $s` -lt 60 ] || fail "run_command timeout took `expr $e - $s` s" ;;
esac

if [ -n "$M" ]; then
    "$G" m2 >/dev/null 2>&1
    if has 'says hi' $LOG; then ok "MCP on stdio"; else fail "MCP on stdio"; fi
fi

if [ -d lnk ]; then
    (cd lnk && "$G" w1 >/dev/null 2>&1)
    if [ -f sub/w1.txt ]; then ok "write through a symbolic link"; else fail "write through a symbolic link"; fi
fi

out=`"$G" slow 2>&1`
case $out in *timeout*) ok "request timeout" ;; *) fail "request timeout: $out" ;; esac

kill $fpid 2>/dev/null
cd /
if [ $fails -eq 0 ]; then
    echo "all checks passed"
    rm -rf $T
    exit 0
fi
echo "$fails check(s) failed; the fake's log is in $LOG"
exit 1
