# Working on gork

For whoever changes the source. [CONTRIBUTING.md](../CONTRIBUTING.md) says
how to send the result, and [MAINTAINING.md](MAINTAINING.md) how releases
are made.

## Layout

| Path | What |
|---|---|
| [main.c](../main.c) | Agent loop, tool table, request/response handling |
| [relay.c](../relay.c), [relay.h](../relay.h) | HTTP POST through the relay; growable buffer |
| [tui.c](../tui.c), [tui.h](../tui.h) | Interactive chat pane, input line, approvals |
| [mcp.c](../mcp.c), [mcp.h](../mcp.h) | MCP client: stdio servers, JSON-RPC, tool calls |
| [port.h](../port.h), [win.c](../win.c) | What differs without `fork` (DOS, Windows); the Windows replacements for POSIX calls |
| [Makefile.dj](../Makefile.dj), [dosbuild.sh](../dosbuild.sh), [Makefile.w32](../Makefile.w32), [winbuild.sh](../winbuild.sh) | The DOS (DJGPP) and Windows (MinGW) cross builds |
| [cygpack.sh](../cygpack.sh) | Packs the Cygwin build as the `win32` zip |
| [dos16build.sh](../dos16build.sh), [dos16/](../dos16/) | The 16-bit DOS build (Open Watcom), and two generated files Watt-32 needs for it |
| [dosspx.c](../dosspx.c) | SPX on DOS, through the IPX driver |
| [nt3crt.c](../nt3crt.c) | What `gorknt3.exe` needs to use `crtdll.dll` |
| [configure.ac](../configure.ac), [Makefile.am](../Makefile.am) | Build: curses detection, compiler flags, sources |
| [cJSON/](../cJSON/) | Vendored JSON parser |
| [docs/BUILD.md](BUILD.md) | Build instructions per operating system |
| [testing/](../testing/) | Fakes and checks |
| [gork.spec](../gork.spec), [debian/](../debian/) | RPM and deb packaging |
| [Jenkinsfile](../Jenkinsfile), [release.sh](../release.sh) | CI: tarball and packages; tag builds publish releases |
| [images/](../images/) | Icon, installed to the hicolor theme |

## The loop

One turn is: build the request from the conversation so far, POST it, then
branch on `stop_reason`.

- `tool_use` -- run every `tool_use` block in the reply, send **all** results
  back in a single user turn, and go again. Splitting results across turns
  teaches the model to stop calling tools in parallel; omitting one is
  rejected outright.
- anything else -- print the text and stop.

Two details the loop depends on, both easy to break by "tidying":

- The assistant turn is echoed back **verbatim** (`cJSON_Duplicate`), tool_use
  and thinking blocks included. Editing it gets the next request rejected.
- No `thinking` field is sent, so the model runs its default adaptive
  thinking. With thinking disabled it will occasionally write a tool call into
  its *visible text* instead of emitting a `tool_use` block -- the turn
  succeeds, the call never runs, and nothing reports an error. `max_tokens`
  caps thinking and response text together.

`max_turns` bounds the loop so a model stuck on a failing tool burns a
bounded number of requests. A request that fails with a network error, 429
or 5xx is retried up to `retries` times, waiting 2, 4, 8, ... seconds (at
most 60) in between; any other error ends the task at once.

## Adding a tool

Write the function, then add a row to `TOOLS[]` in [main.c](../main.c):

```c
static char *tool_read_file(cJSON *input, int *is_err);

static const struct tool TOOLS[] = {
    { "read_file",                                  /* name          */
      "Read a file from disk and return its contents.",  /* description */
      "{\"type\":\"object\", ... }",                /* input_schema  */
      tool_read_file }
};
```

A tool returns freshly `malloc`'d result text (the caller frees it) and sets
`*is_err` on failure, which flags the `tool_result` block so the model can
recover instead of trusting a bad result. Returning `NULL` means out of memory
and aborts the run.

The schema string is parsed with cJSON at request time, so a malformed one
fails the build of the request, not the compile -- keep it valid.

## Testing

```sh
python3 testing/looptest.py ./gork
```

Stands up a fake API, drives gork through a full `tool_use` round-trip, and
asserts the wire shape: headers, tool declaration, the verbatim assistant
echo, and one correctly-matched `tool_result` in a single user turn. This is
the check that fails if the loop breaks.

`testing/oldvm.sh build` makes a Debian 1.3 (1997: Linux 2.0.33, libc5,
gcc 2.7.2.1) disk image for QEMU, and `testing/oldvm.sh run` boots it with
the current tree as a tarball on `/dev/hdb`; `./build.sh` inside builds it.
The header of the script has the details.

`testing/wintest.py` checks the Windows `gork.exe` in batch mode, under
Wine or on Windows. `testing/dostest.py` does the same for DOS: it boots FreeDOS in QEMU, runs
the `gork.exe` that `sh dosbuild.sh` built, and checks a round trip, path
confinement, an `allow` directory on a second drive, `edit_file`,
`run_command`, long output, the timeout and the TUI. Jenkins runs it on
every build, for the 32-bit and the 16-bit zip. With `GORK_RELAY` naming a
[gork-relay](https://github.com/yeold/gork-relay) binary it checks SPX too;
its header lists what else it can boot (other DOS versions, Windows 1 to
3.1).

`testing/cygpacktest.py` checks the Cygwin package on a Windows without
Cygwin.

`testing/fakemcp.py` is the stdio MCP server the MCP check runs against.
