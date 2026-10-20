# Building gork on OS/2

Back to [all systems](../BUILD.md).

**OS/2 Warp 4.52** (EMX 0.9d fix 04, gcc 2.8.1, PDCurses 3.4). Unzip
`emxrt`, `emxdev1`, `emxdev2`, `gnudev1`, `gnudev2` and then `emxfix04` in
the root of a drive (they are on SourceForge, project `emx`), and set the
environment, here in `STARTUP.CMD`:

```bat
SET BEGINLIBPATH=C:\EMX\DLL
SET PATH=C:\EMX\BIN;%PATH%
SET C_INCLUDE_PATH=C:/EMX/INCLUDE
SET LIBRARY_PATH=C:/EMX/LIB
```

There is no `make`; build PDCurses's OS/2 port by hand, in its `os2`
directory. `-x c` matters when the files came from a CD: their names are
then upper case, and gcc takes `.C` for C++.

```bat
gcc -x c -O2 -I.. -c ../pdcurses/*.c *.c
ar rc pdcurses.a *.o
```

Then gork, with PDCurses in `C:\PDCURSES`:

```bat
gcc -O -Ic:/pdcurses -o gork.exe main.c relay.c tui.c mcp.c cJSON/cJSON.c c:/pdcurses/os2/pdcurses.a -lsocket
```

The configuration is `%HOME%/.gork.conf`, and the trust prompt and skills
need `HOME` too; OS/2 does not set it. That name needs HPFS: on FAT, set
`GORK_CONFIG` to an 8.3 name instead. `run_command` runs `%COMSPEC%`
(`CMD.EXE`) and cannot be interrupted, only timed out. Batch mode, the TUI
and MCP over HTTP work, tested in QEMU from a text-mode boot
(`PROTSHELL=C:\OS2\CMD.EXE`): Warp 4.52's generic VESA driver crashes
there, so the desktop was never up.

**OS/2 Warp 3** builds the same way, with the same EMX and PDCurses. It
has to be Warp Connect: plain Warp 3 has only the dial-up stack of the
Internet Access Kit. Its TCP/IP comes with MPTS (`C:\MPTN\DLL` holds
`so32dll.dll` and `tcp32dll.dll`), and its `gethostbyname()` does not take
a numeric address, so on OS/2 gork tries `inet_addr()` first. Here
`C:\EMX\DLL` went on `LIBPATH` in `CONFIG.SYS`, and the gork command
had `-x c` before the sources and `-x none` before `pdcurses.a`, as the
files came from a CD. Tested the same way as 4.52, from a text-mode
boot. Its `CMD.EXE` ignores the number in `exit 3`, so `run_command`
reports a different exit code there.
