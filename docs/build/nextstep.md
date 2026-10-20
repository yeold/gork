# Building gork on NeXTSTEP and OPENSTEP

Back to [all systems](../BUILD.md).

**OPENSTEP 4.2** for Intel, with the developer tools (NeXT's cc 744.13,
based on gcc 2.7.2.1). Its curses is the 4.3BSD one, so build ncurses
first. ncurses 4.2 works, but its scripts need a newer awk than the system
has: build mawk (1.3.3) first, and have it first in the PATH as `awk`
while ncurses is configured and built.

```sh
# in the ncurses-4.2 source tree, with mawk as awk in /usr/local/bin
PATH=.:/usr/local/bin:$PATH; export PATH
./configure --prefix=/usr/local --without-cxx --without-ada
make && make install
```

Then gork, by hand, with `nextstep.c`: the POSIX headers (`-posix`)
declare calls the library does not have, and its `readdir` and
`realpath` do not match them.

```sh
for f in main relay tui mcp nextstep cJSON/cJSON; do
    cc -posix -O -I/usr/local/include -c $f.c
done
cc -posix -o gork main.o relay.o tui.o mcp.o nextstep.o cJSON.o \
    -L/usr/local/lib -lncurses -lm
```

ncurses has no entry for the console (`TERM=NeXT`); compile the one in
`/etc/termcap` with `sed -n 8,10p /etc/termcap > nx.cap; tic nx.cap`.
Batch mode, the TUI and MCP (over HTTP, and on stdio) work, tested in
86Box on a text console. On that console the arrow and page keys send
nothing ncurses can use, so history and scrolling need a terminal over
telnet. A command cannot be interrupted with Esc.

**NeXTSTEP 3.3** for Intel builds the same way, with the Developer CD's
`DeveloperTools` and `DeveloperLibs` packages (unpack each with
`uncompress -c X.tar.Z | /NextAdmin/Installer.app/installer_bigtar xpBf -`
from `/`). Its `-posix` library differs from OPENSTEP's: `getwd` and
`realpath` fail outright and `readdir` returns the POSIX record;
`nextstep.c` handles both. Its `mkdir` has no `-p`. Batch mode, the TUI
and MCP on stdio work, tested in 86Box over loopback against
`testing/fakeapi.c`: its installer knows only SCSI (an Adaptec 1542C),
and 86Box's WD8013 card with its `SMC16` driver passes no TCP to the
host. With a host name set in `/etc/hostconfig`, or once a network is
up, NetInfo times out on every lookup, logins included.
