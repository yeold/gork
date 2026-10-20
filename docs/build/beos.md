# Building gork on BeOS

Back to [all systems](../BUILD.md).

**BeOS R5** for Intel (5.0, the free Personal Edition), with Be's 5.0.1
developer tools (gcc 2.9-beos-991026): unzip them into `/boot`. There is
no curses, so build ncurses 5.9 first. Its `bool` must be BeOS's
(`unsigned char`) or `<OS.h>` clashes with it: after `configure`, change
`typedef char NCURSES_BOOL` to `typedef unsigned char NCURSES_BOOL` in
`include/curses.head` and `include/curses.h`.

```sh
./configure --prefix=/boot/home/config --without-cxx --without-cxx-binding \
    --without-ada --without-debug --without-tests
make && make install
```

Then gork, by hand, with `beos.c`: libroot has no `realpath`, and the
`select` of R5's network stack takes sockets only, so pipes are polled.

```sh
for f in main relay tui mcp beos cJSON/cJSON; do
    gcc -O -I/boot/home/config/include -I/boot/home/config/include/ncurses -c $f.c
done
gcc -o gork *.o -L/boot/home/config/lib -lncurses -lnet
```

Batch mode, the TUI and MCP (over HTTP, and on stdio) work, tested in
QEMU in the Terminal app (`TERM=beterm`). BeOS has no job control, so a
command cannot be interrupted with Esc; its timeout still ends it.
