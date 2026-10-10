# <img src="images/gork.png" width="64" alt=""> gork

A general-purpose AI agent harness for machines that predate TLS.

gork runs an agent's tool-use loop -- send a task, get tool calls back,
execute them locally, feed the results in, repeat -- in strict C89 with no
dependencies beyond libc, curses and vendored cJSON. It targets gcc 2.7.2+,
libc 5.4 or glibc 2.0+, and Linux 2.0+, and also builds on Solaris (configure
adds `-lsocket -lnsl` and uses the system curses) and Haiku (`-lnetwork`,
with the `ncurses6_devel` package).

It talks to any server that speaks the Messages API format: Anthropic's own
API, or a local model behind LM Studio or a similar compatible server (see
[Local models](#local-models)). It speaks plaintext HTTP/1.0 only;
reaching an HTTPS endpoint is the relay's job, not gork's.

## Why a relay

Modern TLS is not reachable from the machines gork is for: no usable OpenSSL,
no ciphers the API will accept, and nothing worth the effort of backporting.
So gork doesn't try. It POSTs plaintext to a relay host on your network, and
the relay terminates TLS on its behalf.

```
  old box                      modern box                  API server
+-----------+              +----------------+            +-------------+
|   gork   |- plaintext ->|  socat/stunnel |--- TLS --->| /v1/messages|
+-----------+   HTTP/1.0   +----------------+            +-------------+
```

**The relay sees everything in the clear, including your API key.** Run it on
a host and network you trust; it is the security boundary this design trades
away. That is the deal gork makes to run at all on hardware this old.

HTTP/1.0 with `Connection: close` is deliberate: the server then replies
unchunked and closes the socket, so "read until EOF" is complete and correct
framing. That removes the fiddliest part of HTTP from the code.

## Build

gork uses autotools. From a release tarball, or any tree that already has
`./configure`:

```sh
./configure
make
```

From a git checkout, generate `configure` first (needs autoconf and
automake; the ones SuSE 8.1 ships are new enough):

```sh
autoreconf -i
./configure
make
```

Produces `./gork`; `make install` puts it in `/usr/local/bin` (change with
`./configure --prefix=...`). To build on a box with no autotools at all, run
`make dist` on a machine that has them and copy over the resulting
`gork-<version>.tar.gz` -- it ships a ready-made `configure`.

cJSON is vendored in [cJSON/](cJSON/); the only link dependencies are curses
and `-lm`. configure looks for `-lncurses` first, then plain `-lcurses`
(NetBSD's own curses). ncurses 5.x is fine. On a UTF-8 terminal, configure
with `./configure LIBS=-lncursesw` so non-ASCII text renders.

The generated Makefile is plain portable make, so the BSDs' own `make` should
do. Verified on NetBSD 10.1 (x86_64): a fresh clone builds and installs
with `autoreconf -i`, `./configure`, `make` and `make install`, no extra
setup.

### No curses headers

No ncurses headers (e.g. SuSE 8.1 without `ncurses-devel`)? Build a static
ncurses into your home directory -- gork then needs only libc at runtime:

```sh
# in an unpacked ncurses-5.9 source tree
./configure --prefix=$HOME/local --without-shared --without-cxx \
    --without-ada --without-progs --without-tests --without-manpages \
    --enable-overwrite --with-terminfo-dirs=/usr/share/terminfo \
    --with-default-terminfo-dir=/usr/share/terminfo
make libs && make install.libs install.includes
```

Then build gork as usual: configure picks up `~/local` by itself when the
system has no `curses.h`. Anywhere else, point it there by hand:

```sh
./configure CPPFLAGS=-I/path/include LDFLAGS=-L/path/lib
```

Verified on SuSE 8.1 (gcc 3.2, glibc 2.2, Linux 2.4).

### Oldest systems

The oldest system gork is verified on is Debian 1.3 "bo" from 1997
(gcc 2.7.2.1, libc 5.4.33, ncurses 1.9.9e, Linux 2.0.33): it builds from the
release tarball, and batch mode and the TUI both work, driven against a
local model. Debian 2.1 (gcc 2.7.2.3, glibc 2.0.7, ncurses 4.2) and Debian
2.2 (gcc 2.95.2, glibc 2.1.3, ncurses 5.0) build and run too.

On anything this old, build from the tarball: its `configure` runs under
bash 2.0, but autoconf 2.13 and automake 1.4 cannot regenerate it from a git
checkout. gcc 2.7.2 says `unrecognized option -std=gnu89` on every file;
that is harmless. [testing/oldvm.sh](testing/oldvm.sh) sets up the Debian
1.3 machine in QEMU (see [Testing](#testing)).

## Run

Start a relay on a machine that can do TLS:

```sh
socat TCP-LISTEN:8443,fork,reuseaddr OPENSSL:api.anthropic.com:443
```

or, if you'd rather have a long-lived service, stunnel in client mode:

```ini
[anthropic]
client  = yes
accept  = 0.0.0.0:8443
connect = api.anthropic.com:443
```

Then point gork at it in `~/.gork.conf`:

```ini
relay   = relayhost:8443
api_key = sk-ant-...
```

and run a task:

```sh
./gork "what's in main.c?"
```

Agent text goes to stdout; tool calls and diagnostics go to stderr, so
`./gork ... 2>/dev/null` gives you just the answer.

### Interactive

Leave off the task to get a chat TUI, Cline-style:

```sh
./gork
```

The first time you start the TUI in a directory, gork asks whether to trust
it -- before reading anything from it, since `GORK.md` and `.gork/skills`
become instructions to the model:

```
Trust this directory?
  y  yes
  e  yes, and let gork edit files here without asking
  n  no, quit
```

The answer is remembered in `~/.gork/trusted` (`trust <dir>` or
`edits <dir>` lines; edit or delete them to change your mind), and covers
subdirectories too. With `e`, `write_file` and `edit_file` run without a
prompt; commands and MCP tools still ask. Starting in your home directory or
`/` gets an extra warning. Batch mode never asks: naming a task on the
command line is consent enough.

Each message continues the same conversation. Before any `write_file`,
`edit_file` or `run_command`, gork shows what it is about to do and asks: `y` allow, `n`
decline (the model is told and adapts), `a` allow that tool for the rest of
the session. Reads never ask.

Keys: Enter send, PgUp/PgDn scroll, Ctrl-U clear the line, Esc abort the
running task, Ctrl-D (on an empty line) or `/quit` exit. `/new` clears the conversation. A task
that fails (network, API error) is dropped from the conversation so the next
message starts clean; an aborted one is kept, so you can redirect it.

Esc stops waiting for the model at once, and at an approval prompt it
declines and stops. A running command is interrupted; an MCP call that is
already running finishes first.

The status bar shows how full the context is (`ctx 42k/200k (21%)`), from
the token counts the server reports with each reply. `/compact` replaces the
conversation with the model's own summary of it. gork also does this by
itself, in batch mode too, once the context passes `auto_compact` percent of
`context_window` -- between turns, even in the middle of a task.

`/effort high` sets the reasoning effort for the following requests (`low`,
`medium`, `high`, `xhigh`, `max`, or `default` to send none); `/effort` alone
shows it, and so does the status bar. It starts at `default_effort`.

### Local models

LM Studio 0.4.1+ serves a native Anthropic-compatible `/v1/messages`, which is
the same wire format gork already speaks -- so a local model needs **no
adapter and no relay**. It's plaintext HTTP on localhost; the relay only ever
existed to terminate TLS.

Start the LM Studio server, load a model, then use a config like:

```ini
relay = localhost:1234
host  = localhost:1234
model = ibm/granite-4-micro
```

No `api_key` needed -- gork only insists on one when the target is actually
`anthropic.com`. If you've turned on *Require Authentication* in LM Studio,
set `api_key` to the token it gives you; it rides the same `x-api-key` header.

`model` must name a model LM Studio has loaded (`curl
localhost:1234/v1/models` lists them) -- the default `claude-opus-5` means
nothing locally.

The same applies to any other server exposing an Anthropic-compatible
`/v1/messages`. For an **OpenAI-shaped** local server there is no support:
`tools`, `tool_calls`, and `finish_reason` are a different wire format, and
`function.arguments` arrives as a JSON-encoded *string* rather than an object.
That's a real adapter, not a config change -- worth writing only if you need a
server that has no Anthropic-compatible endpoint.

### Config

gork reads `~/.gork.conf`, or the file `GORK_CONFIG` names. One
`key = value` per line; lines starting with `#` are comments. An unknown key
or a malformed line stops gork with the line number rather than being
skipped.

| Key | Env override | Default | Meaning |
|---|---|---|---|
| `relay` | -- | *(required)* | `host:port` gork connects to: the relay, or a local server |
| `api_key` | `ANTHROPIC_API_KEY` | *(required for anthropic.com)* | Sent as the `x-api-key` header |
| `model` | `AGENT_MODEL` | `claude-opus-5` | Model id |
| `host` | `AGENT_HOST` | `api.anthropic.com` | `Host:` header value |
| `path` | `AGENT_PATH` | `/v1/messages` | Request path |
| `allow` | -- | *(none)* | Extra directories the file tools may read and write, `:`-separated (up to 8) |
| `max_turns` | `AGENT_MAX_TURNS` | `24` | Tool round-trips per task before gork gives up |
| `retries` | `AGENT_RETRIES` | `2` | Extra attempts for a request that hits a network error, 429 or 5xx (0..10) |
| `context_window` | `AGENT_CONTEXT_WINDOW` | `200000` | Tokens the model can take; set it to the loaded context length for a local model |
| `auto_compact` | `AGENT_AUTO_COMPACT` | `80` | Compact once the context passes this percentage of `context_window` (0 = never, max 99) |
| `timeout` | `AGENT_TIMEOUT` | `120` | Seconds one API request, MCP tool call or `run_command` may take before it fails |
| `default_effort` | `AGENT_DEFAULT_EFFORT` | *(unset: model default)* | Reasoning effort at startup: `low`, `medium`, `high`, `xhigh` or `max`, sent as `output_config.effort`; `/effort` changes it in the TUI |

A set environment variable beats the file, and gork says so at startup (e.g. `model meta/muse-glimmer from AGENT_MODEL overrides ~/.gork.conf`), since a stale `export` in an old shell is easy to forget. If the file holds `api_key` and
anyone but you can read it, gork refuses to start: `chmod 600 ~/.gork.conf`.

Two more switches are environment-only:

| Variable | Meaning |
|---|---|
| `GORK_TRACE` | Dump response headers and bodies to stderr |
| `GORK_ALLOW_RUN` | Offer `run_command` in batch mode (see [Tools](#tools)) |

### Rules

If `GORK.md` exists in the working directory, its contents are sent as the
system prompt on every request. A missing file is fine; an unreadable or
oversized (>64 KiB) one stops gork rather than running without your rules.

### Skills

A skill is a directory holding a `SKILL.md` -- the same layout Claude Code
and Cline use, so their skills drop straight in:

```markdown
---
name: release
description: >
  Cut a release: bump the version, tag, and build the tarball.
  Use when asked to release or ship.
---
Steps the model should follow...
```

gork loads skills from `.gork/skills/<name>/` in the working directory and
from `~/.gork/skills/<name>/`; a project skill wins over a user skill of the
same name. Only each skill's name and description go into the system prompt
(after `GORK.md`); the model calls the `skill` tool to load the full
instructions when a task matches. A skill without a description is skipped
with a warning.

The frontmatter parser understands one-line values and `>` / `|` blocks,
which is what skills use in practice -- not YAML in general.

### MCP servers

gork is an MCP client for **stdio** servers: local programs it starts and
talks to over their stdin/stdout. List them in `~/.gork/mcp.json`, in the
same `mcpServers` shape Claude Code and Cline use, so existing entries can be
copied over:

```json
{
  "mcpServers": {
    "notes": {
      "command": "python3",
      "args": ["/opt/mcp/notes_server.py"],
      "env": { "NOTES_DIR": "/home/me/notes" },
      "autoApprove": ["search"]
    }
  }
}
```

At startup gork runs each server, asks for its tools, and offers them to the
model as `mcp__<server>__<tool>`. In the TUI every MCP call asks for approval
first, unless the tool is in that server's `autoApprove` list or you've
answered `a`. `"disabled": true` skips a server. `"tools": [...]` offers
only the named tools -- every tool's schema goes out with every request, and
some servers list enough to overflow the model's context on their own.

A server that fails to start or doesn't answer within 30 s is reported and
skipped; the others still load. A call that takes longer than `timeout`
(120 s) fails with an error the model sees, and output past 64 KiB is cut
off. Server stderr is discarded unless
`GORK_TRACE` is set, so its logging doesn't flood the TUI.

HTTP servers (`"url": ...`) are skipped: they are mostly TLS-only, which old
machines can't speak. Only tools are supported -- not MCP resources, prompts,
or sampling. Non-text tool output (images, audio) is replaced by a note.

### Tools

| Tool | Default | What |
|---|---|---|
| `list_dir` | on | List a directory, optionally recursive (sizes, `/` dirs, `@` symlinks) |
| `read_file` | on | Read a file (up to 64 KiB) |
| `write_file` | on | Create or overwrite a file |
| `edit_file` | on | Replace one exact snippet; refused if it matches zero or several times |
| `run_command` | batch: **off**, TUI: on | Run a `/bin/sh` command; returns stdout+stderr and `[exit N]` |
| `skill` | on when skills exist | Load a skill's `SKILL.md` |
| `mcp__<server>__<tool>` | from `~/.gork/mcp.json` | Tools from MCP servers (see [MCP servers](#mcp-servers)) |

The file tools are confined to the working directory plus any `allow`
directories from the config: paths are resolved through symlinks and `..`,
and anything landing outside is refused. An `allow` directory that doesn't
exist stops gork at startup, and the model is told which extra directories
it has. `list_dir` never follows symlinks and doesn't descend into hidden
directories, and stops at 8 levels deep or 2000 entries. `read_file` may also read inside
`~/.gork/skills`, so a skill's
extra files are reachable; nothing can write there.

```ini
allow = /home/me/projects:/srv/data
```

`write_file` and `edit_file` save through a temp file and a rename, so a
failed write never leaves a half-written file, and an existing file keeps its
permissions. `edit_file` works on files up to 1 MiB; `read_file` returns up
to 64 KiB.

`run_command` cannot be confined -- the shell reaches whatever your user can --
so in batch mode it is only offered when `GORK_ALLOW_RUN` is set. The TUI
always offers it, because it asks before every command. Its stdin is
`/dev/null`, so a command that reads stdin fails instead of hanging; one that
prompts on `/dev/tty` (ssh, ftp, sudo passwords) still reaches you, since the
TUI hands the terminal back while the command runs. Output is capped at
64 KiB, and a nonzero exit is flagged as an error to the model.

A command that runs past `timeout` is killed, along with everything it
started (it runs in its own process group). Esc in the TUI, or Ctrl-C in
batch mode, interrupts the command and aborts the task. While a command runs,
Esc is the terminal's interrupt key, so arrow keys typed at a prompt
interrupt it too.

## Layout

| Path | What |
|---|---|
| [main.c](main.c) | Agent loop, tool table, request/response handling |
| [relay.c](relay.c), [relay.h](relay.h) | HTTP POST through the relay; growable buffer |
| [tui.c](tui.c), [tui.h](tui.h) | Interactive chat pane, input line, approvals |
| [mcp.c](mcp.c), [mcp.h](mcp.h) | MCP client: stdio servers, JSON-RPC, tool calls |
| [configure.ac](configure.ac), [Makefile.am](Makefile.am) | Build: curses detection, compiler flags, sources |
| [cJSON/](cJSON/) | Vendored JSON parser |
| [testing/](testing/) | Fakes and checks |
| [gork.spec](gork.spec), [debian/](debian/) | RPM and deb packaging |
| [Jenkinsfile](Jenkinsfile), [release.sh](release.sh) | CI: tarball and packages; tag builds publish releases |
| [images/](images/) | Icon, installed to the hicolor theme |

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

Write the function, then add a row to `TOOLS[]` in [main.c](main.c):

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

`testing/fakemcp.py` is the stdio MCP server the MCP check runs against.
[testing/](testing/) also holds the milestone-0 pieces: `relaytest.c` (a
standalone transport probe that POSTs a body from stdin), `fakeapi.py` (a
minimal echo server), and `req.json` (a sample request body).

## Known gaps

- **Esc can't stop a running MCP call.** It waits for the call to finish
  (or hit `timeout`), then aborts.
- **Basic line editor.** No cursor movement, history, or multi-line input.
- **The TUI is not covered by `looptest.py`.** It was checked by hand under
  tmux.
- **Path confinement is check-then-open.** A symlink swapped in between can
  escape it. That stops a model, not a hostile local user.
- **No streaming.** Replies are read to EOF, so a long generation is a long
  silence. Fine for a batch harness, poor for interactive use -- and more
  noticeable against a slow local model than against the API.
- **No chunked decoding.** gork asks for HTTP/1.0 so replies come back
  unchunked. A server that chunks anyway is detected and reported by name
  rather than failing as a JSON error; decoding it would be ~30 lines in
  `relay.c`.
- `relaytest.c` still carries its own copy of the transport rather than
  linking `relay.c`.
