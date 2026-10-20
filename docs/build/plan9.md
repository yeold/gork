# Building gork on Plan 9

Back to [all systems](../BUILD.md).

**9front** (release 11952, amd64), through APE, its POSIX environment.
There is no curses, so `notui.c` takes the place of `tui.c` and only
batch mode is built; `plan9.c` supplies `realpath`. `pcc` predefines
nothing of its own, hence `-DGORK_PLAN9`:

```sh
for f in main relay mcp notui plan9 cJSON/cJSON; do
    pcc -DGORK_PLAN9 -D_POSIX_SOURCE -D_BSD_EXTENSION -D_SUSV2_SOURCE \
        -D_C99_SNPRINTF_EXTENSION -c $f.c
done
pcc -o gork main.6 relay.6 mcp.6 notui.6 plan9.6 cJSON.6
```

(in `rc`, the loop is `for(f in main relay ...){ ... }`). Run gork from
`ape/psh`: it sets `HOME` from `$home`, and puts APE's `sh` at `/bin/sh`,
which `run_command` and MCP servers written in sh want (from plain `rc`,
`run_command` falls back to `/bin/ape/sh`). The fake API in `testing/`
needs a loopback interface, which 9front does not set up by itself:
`ip/ipconfig loopback /dev/null 127.1`. Batch mode, MCP on stdio and over
HTTP work, tested in QEMU with 9front's own qcow2 image, on the serial
console.
