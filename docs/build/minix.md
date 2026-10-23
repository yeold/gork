# Building gork on Minix

Back to [all systems](../BUILD.md).

**Minix 2.0.4** (i386, the ACK compiler). Install the `USR`, `NET` and
`SYS` sets; `NET` brings the TCP/IP server, which needs `servers=inet`
and the network card (`DPETH0=300:9:0` for an NE2000) in the boot
parameters. There is no `configure` route; compile by hand, with
`minix2.c` for what the system lacks:

```sh
for p in em_cemcom.ansi em_opt i386/cg i386/as em_led; do
    chmem =8000000 /usr/lib/$p
done
for f in main relay tui mcp minix2 cJSON/cJSON; do
    cc -D_POSIX_SOURCE -D_MINIX -c $f.c
done
cc -o gork main.o relay.o tui.o mcp.o minix2.o cJSON.o -lcurses -lm
chmem =4000000 gork
```

Programs there get a fixed amount of memory: without the first `chmem`
the compiler runs out on `main.c`, and without the last one gork does in
the TUI. Batch mode, the TUI and MCP (over HTTP, and on stdio with a
server written in `sh`) work, tested in QEMU. Without `select`, Esc does
not stop a reply that is under way (the timeout still does), and a
command cannot be interrupted. Names on its file system stop at 14
characters, so gork writes a file through a temporary file called
`.gork-tmp` beside it.

**Minix 3.3.0.** The base system has make and curses but no compiler.
Install `clang` and `binutils` from the 3.3.0 package set (`pkgin`, or
`pkg_add -f` on the downloaded files; do it under `/usr`, the root file
system is too small), then build from the tarball:

```sh
./configure CC=clang
make
```

Batch mode and the TUI work.
