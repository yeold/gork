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
| [bareboxbuild.sh](../bareboxbuild.sh), [barebox/](../barebox/) | The barebox build: barebox with a `gork` command, gork's half built against `fwinclude/` with its C library's names made `gork_*` (`rename.py`, `gork.lds`), barebox's half the command (`glue.c`), the two meeting in `bb.h`; `libc.c` is gork's half of the C library |
| [grubbuild.sh](../grubbuild.sh), [grub/](../grub/) | The GRUB build: GRUB for x86-64 UEFI with a `gork` module, gork's half the barebox build's, GRUB's half the command (`glue.c`), with `(gork)`, a file system in RAM |
| [fwlibc.c](../fwlibc.c), [fwinclude/](../fwinclude/), [ip.c](../ip.c) | Shared by the firmware ports: strings, numbers, `printf`, stdio and dates; the standard headers; gork's own TCP/IP, on a network card the port gives it (`ip.h`) |
| [uefibuild.sh](../uefibuild.sh), [uefi/](../uefi/) | The UEFI build: `gorkx64.efi` and `gorkaa64.efi`, with a C library of its own (`libc.c`), sockets on the firmware's TCP4 (`net.c`) or on gork's own TCP/IP for firmware without (`../ip.c`, on the card through `card.c`), and the standard headers |
| [nt3crt.c](../nt3crt.c) | What `gorknt3.exe` needs to use `crtdll.dll` |
| [configure.ac](../configure.ac), [Makefile.am](../Makefile.am) | Build: curses detection, compiler flags, sources |
| [cJSON/](../cJSON/) | Vendored JSON parser |
| [docs/BUILD.md](BUILD.md) | Build instructions per operating system; the systems built by hand have their own guide in [docs/build/](build/) |
| [testing/](../testing/) | Fakes and checks; [docs/TODO-VMS.md](TODO-VMS.md) is the plan for scripts that rebuild the test VMs |
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

With `"stream": true` the reply comes as server-sent events instead.
`relay.c` hands the body over as it arrives (`relay_stream`), and the
`stream_*` functions in `main.c` print the text and put the pieces back
together into the message a whole reply would have been, since that is
what has to go back verbatim: text, thinking with its signature, and tool
calls whose JSON arrives in parts. For `protocol = openai` the chunks are
gathered into a whole chat completion and converted as one. A reply that
turns out to be plain JSON is taken as it is.

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
the check that fails if the loop breaks. With `GORK_RELAY` naming a
[gork-relay](https://github.com/yeold/gork-relay) binary, every fake API
sits behind it. A build made `--without-tui` or `--without-mcp` skips the
checks it cannot run, and says so.

`testing/oldvm.sh build` makes a Debian 1.3 (1997: Linux 2.0.33, libc5,
gcc 2.7.2.1) disk image for QEMU, and `testing/oldvm.sh run` boots it with
the current tree as a tarball on `/dev/hdb`; `./build.sh` inside builds it.
The header of the script has the details.

`testing/wintest.py` checks the Windows `gork.exe` in batch mode, under
Wine or on Windows, and under Wine the TUI as well. `testing/dostest.py` does the same for DOS: it boots FreeDOS in QEMU, runs
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

`testing/fakeapi.c` is a fake Messages API in plain C89, for guests that
cannot reach the host or run Python (AIX PS/2 used it over loopback). It
listens on 127.0.0.1 (port 8099, or the first argument); a turn starting
with "go" gets `list_dir`, `write_file`, `read_file` and `run_command`
calls, "w1" a write, "t1" `sleep 100`, "m1" a call to the MCP server
named `fake` (`fakemcp.py --http`), "m2" one to the MCP server named
`sh`, "esc" two paths outside the working directory, and "slow" waits
30 s; anything else, or a turn of tool results, a short text reply.
Where the loopback address cannot be bound (Plan 9's APE) it listens on
all addresses. Request bodies go to `fakeapi.log`.

`testing/vmcheck.sh GORK FAKEAPI [MCPSH]` runs gork's batch checks against
that fake on a system with only a Bourne shell and its base tools (a
reply, the file tools, `run_command` and its timeout, paths outside the
working directory, a symbolic link, the request timeout, and MCP on stdio
through `testing/mcp.sh`, an MCP server in sh). It works under `/tmp` with
`HOME` pointed there.

`uefibuild.sh [OUT]` builds gork for x86-64 and arm64 UEFI with clang
and lld, and `testing/uefitest.py ZIP FAKEAPI [MACHINE]` runs it from the
UEFI shell in QEMU (OVMF, AAVMF, OVMF without its network stack, U-Boot), its checks in a `startup.nsh` on a FAT image, against
`fakeapi.c` on the host.

`bareboxbuild.sh [OUT] [WORK]` builds barebox for QEMU's arm64 `virt`
machine with a `gork` command in it, and `testing/bareboxtest.py ZIP
FAKEAPI` boots it in QEMU and types its checks at barebox's prompt
(docs/build/barebox.md).

`grubbuild.sh [OUT] [WORK]` builds GRUB for x86-64 UEFI with a `gork`
module in it, and `testing/grubtest.py ZIP FAKEAPI` boots it under OVMF
in QEMU and types its checks at GRUB's prompt (docs/build/grub.md).

`bsdbuild.sh [OUT] [WORK]` cross-builds the FreeBSD, NetBSD and OpenBSD
packages on Linux with clang, against sysroots unpacked from each
release's base sets, and the fake API beside each. `testing/bsdtest.py
TARGET PACKAGE FAKEAPI` boots the release's own VM image in QEMU
(OpenBSD's is installed once, unattended, from its install image), puts
the package on a CD and runs `vmcheck.sh` there on the serial console.
The disks are kept in `$GORK_VM` as compressed qcow2 and the downloads
dropped, as `bsdbuild.sh` drops the sets once unpacked; everything is
checksum-pinned.
