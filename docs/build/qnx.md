# Building gork on QNX

Back to [all systems](../BUILD.md).

**QNX 6.2.1** (Neutrino), the free Non-Commercial Edition. Its base
install has no compiler: the packages are on the CD under `rep621`. From
there `binutil`, `gcc`, `make` and `os-dev`/`os-devtools` (6.2.0's
2.1.3 first, then 6.2.1's 2.1.4 over them) bring gcc 2.95.3 as
`ntox86-gcc`, the headers and libraries, and ncurses. gcc wants the linker
as `ntox86-ld`; `ln -s /usr/bin/ld /usr/bin/ntox86-ld`. Then gork, by
hand:

```sh
for f in main relay tui mcp cJSON/cJSON; do ntox86-gcc -O -c $f.c; done
ntox86-gcc -o gork main.o relay.o tui.o mcp.o cJSON.o -lncurses -lsocket -lm
```

Batch mode, the TUI (in Photon's terminal) and MCP (over HTTP, and on
stdio) work, tested in QEMU. QNX's terminal type, `qansi`, shows dim text
as black, so gork uses plain text there instead.
