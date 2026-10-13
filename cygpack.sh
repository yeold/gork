#!/bin/sh
# Pack the gork.exe just built under Cygwin as OUT/gork-<version>-win32.zip,
# with what it needs to run on a Windows that has no Cygwin: the Cygwin
# DLLs, a /bin/sh for run_command and MCP servers (dash), and the terminal
# descriptions curses reads.  Run in a Cygwin shell, in the source tree,
# after make; needs zip, dash and ncurses:  sh cygpack.sh [OUT]
#
# The directory with cygwin1.dll is named bin, so Cygwin takes its parent
# as /: /bin/sh, /usr/share/terminfo and /tmp are found inside the package.
set -eu
OUT=${1:-out}
ver=$(sed -n 's/^AC_INIT(\[gork\], \[\(.*\)\])/\1/p' configure.ac)
d=gork-$ver-win32

rm -rf "$d"
mkdir -p "$OUT" "$d/bin" "$d/tmp"
OUT=$(cd "$OUT" && pwd)
strip -o "$d/bin/gork.exe" gork.exe
cp /usr/bin/dash.exe "$d/bin/sh.exe"
for f in "$d/bin/gork.exe" "$d/bin/sh.exe"; do ldd "$f"; done |
    awk '$3 ~ /^\/usr\/bin\// { print $3 }' | sort -u |
    while read -r dll; do cp "$dll" "$d/bin/"; done
# cygwin is what TERM is in a Windows console; the others for ssh and
# terminals that set their own.
for t in cygwin xterm xterm-256color vt100; do
    f=$(find /usr/share/terminfo -name "$t" | head -n 1)
    mkdir -p "$d${f%/*}"
    cp "$f" "$d$f"
done
echo "gork's temporary files; Cygwin's /tmp." > "$d/tmp/README.txt"

# Started through this, so that the config is looked for in the user's
# profile: Cygwin's own idea of the home directory is /home/NAME, which
# is not there.
printf '%s\r\n' '@echo off' \
    'rem gork for Windows (the Cygwin build).  Config: %USERPROFILE%\.gork.conf' \
    'if not defined HOME set HOME=%USERPROFILE%' \
    '"%~dp0bin\gork.exe" %*' > "$d/gork.cmd"

cat > "$d/WINDOWS.txt" <<EOF
gork $ver for Windows 10 and later, 64-bit.  Start it with gork.cmd, from
a console window or Windows Terminal.

Config:   %USERPROFILE%\\.gork.conf   (see README.md)
Commands: run_command and MCP servers are started by bin\\sh.exe, a POSIX
          shell.  Windows programs on the PATH can be run from it; the
          Unix tools (ls, grep and so on) are there only if you have
          them, from Cygwin or Git for Windows, on the PATH.
Keep the directories as they are: bin\\gork.exe looks for usr\\ and tmp\\
beside bin\\, and writes its temporary files to tmp\\.

For Windows 95 to 8, or for a single small .exe, there is
gork-$ver-win32-legacy.zip.  That build cannot start MCP servers.

This package contains, unchanged, from the Cygwin distribution
(https://cygwin.com/, sources at https://cygwin.com/packages/):
$(cygcheck -c cygwin dash libncursesw10 2>/dev/null | sed -n '3,$p' | awk '{ print "  " $1 " " $2 }')
cygwin1.dll is under the LGPL, version 3 or later; dash and ncurses under
BSD and MIT style licences.  gork itself: LICENSE.
EOF
cp README.md docs/BUILD.md LICENSE "$d/"
rm -f "$OUT/$d.zip"
zip -qr "$OUT/$d.zip" "$d"
echo "$OUT/$d.zip"
