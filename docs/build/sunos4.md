# Building gork on SunOS 4

Back to [all systems](../BUILD.md).

**SunOS 4.1.4** (Solaris 1.1.2) on SPARC. The bundled `cc` is pre-ANSI,
so build gcc with it first: gcc 2.95.3, C only, still bootstraps from it.
Sun's preprocessor wants `#` in the first column, and one `#endif` in
`gcc/function.c` (line 6930) is indented: move it to the left. There is
no `makeinfo`, so create empty `gcc/cpp.info` and `gcc/gcc.info` before
`make`.

```sh
./configure --prefix=/usr/local sparc-sun-sunos4.1.4
make LANGUAGES=c && make install LANGUAGES=c
```

Its curses is too old, so add ncurses 5.9. Sun's `make` cannot run its
Makefiles and the system `nawk` cannot run its scripts, so build GNU make
(3.79.1; the configure script of 3.81 breaks SunOS's `sh`) and mawk
(1.3.3) with the new gcc first, the mawk with `NO_SPRINTF_IN_STDIO`
deleted from its `config.h`.

```sh
PATH=/usr/local/bin:$PATH; export PATH
AWK=mawk CC=gcc ./configure --build=sparc-sun-sunos4.1.4 --prefix=/usr/local \
    --without-cxx --without-cxx-binding --without-ada --without-tests
gmake && gmake install
```

Then gork, by hand, with `sunos4.c`:

```sh
for f in main relay tui mcp sunos4 cJSON/cJSON; do
    gcc -O -I/usr/local/include -I/usr/local/include/ncurses -c $f.c
done
gcc -o gork *.o -L/usr/local/lib -lncurses -lm
```

The headers declare little, so gcc warns about implicit declarations;
those are harmless. Batch mode, the TUI and MCP (over HTTP, and on stdio)
work, tested in QEMU's SPARCstation 5 on the serial console.
