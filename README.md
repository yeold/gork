# <img src="images/gork.png" width="64" alt=""> gork

A lightweight AI agent harness for old and obscure operating systems.

gork is an AI agent for the machines everything else has left behind. It
runs the usual tool-use loop -- send a task, get tool calls back, execute
them locally, feed the results in, repeat -- on systems from 1987 to today:
MS-DOS 3.30 and Windows 1.04, Linux 1.0, the first BSDs, Solaris 2.5.1,
Windows 95, Minix, GNU/Hurd and Haiku, and just as well on a current
Linux, macOS or Windows 11. One source for all of them, with a batch mode
and a full-screen interactive one. The [table below](#tested-systems) says what
it has been run on.

It is small: about 5,000 lines of strict C89 and a binary of about 100 KB
(x86-64 Linux, stripped), with no dependencies beyond libc, a curses
library and the vendored cJSON.
It builds with gcc 2.4.5, with a single compiler command where `configure`
cannot run, and asks the system for little: TCP sockets, a terminal, a
file system. No TLS, no threads, no C99.

It talks to any server that speaks the Messages API format: Anthropic's own
API, or a local model behind LM Studio or a similar compatible server (see
[Local models](#local-models)). With `protocol = openai` it speaks the
OpenAI chat completions format instead. It speaks plaintext HTTP/1.0
only; reaching an HTTPS endpoint is the job of a [relay](#why-a-relay),
not gork's.

## Tested systems

One row per family: the oldest release gork is verified on, what else was
tried, and where it stops. "Works" means it builds and runs; nearly all were
checked with a tool-use round trip in batch mode and in the TUI, and
openSUSE, Alpine, Debian 2.2, FreeBSD 15.1, OpenBSD 7.9, OmniOS,
GNU/Hurd, DragonFly, MidnightBSD, NetBSD 11.0, macOS and Cygwin pass all
of `testing/looptest.py`.

| Family | Oldest that works | Also works | Does not work |
|---|---|---|---|
| Linux | Slackware 2.0.0 (1994: Linux 1.0.9, gcc 2.5.8, libc 4.5.26, ncurses 1.8.5) | Slackware 3.0, Debian 1.3, 2.1 and 2.2, SuSE 8.1, openSUSE Tumbleweed (gcc 16), Alpine 3.24 (musl), uClibc (static, gcc 15) | Slackware 1.01 (libc 4.4.1): no `realpath`, no `has_colors` |
| FreeBSD | 2.0 (1994, gcc 2.6.2) | 2.0.5, 15.1 | 1.0 and 1.1.5.1: no `realpath` in libc |
| NetBSD | 1.0 (1994, gcc 2.4.5), with ncurses 4.2 added | 10.1, 11.0 | |
| OpenBSD | 2.0 (1996, gcc 2.7.2.1) | 7.9 | |
| DragonFly BSD | 6.4.2 (gcc 8.3) | | |
| MidnightBSD | 4.0.8 (clang 19) | | |
| Solaris | 2.5.1 (1996, gcc 2.7.2) | 2.6, 10 | 2.4: its kernel panics at boot in QEMU, so untested |
| illumos | OpenSolaris build 134 (2010, gcc 3.4.3) | OmniOS r151058, with the system curses or ncurses | |
| Minix | 3.3.0 (2014, clang 3.4) | | |
| GNU/Hurd | Debian GNU/Hurd 2026-03 (gcc 15, glibc 2.42) | | |
| Haiku | R1/beta6 | | Older releases: may work, untested |
| macOS | 26.6 on arm64 (Apple clang 21, system ncurses) | | Older releases: untested |
| Windows (native, experimental; the `win32-legacy` package) | Windows NT 3.1 (1993), with `gorknt3.exe` from the same package; `gork.exe` from NT 3.51 and the first Windows 95 (both 1995), each with `msvcrt.dll` added. Built with MinGW, PDCurses and Winsock 1.1 | Windows 95 OSR2, NT 4.0, 98 SE, Me, 2000, XP SP3, 11, Server 2025; Wine 10 | |
| Windows (Cygwin; the `win32` package) | Cygwin 3.6 on Windows Server 2025 (gcc 14, ncurses 6.5) | | 64-bit Windows 10 and later only. The package was run on GitHub's Windows runner, batch mode only |
| DOS, 32-bit (experimental; the `dos` package) | MS-DOS 3.30 (1988), with DJGPP, PDCurses and Watt-32, 386 or better | MS-DOS 4.01 and 6.22, DR-DOS 7.03, FreeDOS 1.3; in a DOS box under Windows 1.04 (1987, on MS-DOS 3.30), 2.03 and 2.11 (on MS-DOS 4.01), 3.0 and 3.1 (386 enhanced mode) | Windows/386 2.11: use the 16-bit build |
| DOS, 16-bit (experimental; the `dos16` package) | MS-DOS 3.30, real mode, built with Open Watcom for the 8086 | MS-DOS 6.22, FreeDOS 1.3; in a DOS box under Windows 1.04, 2.11/386 and 3.1 | Run on an emulated 486 only: no 8086 or 286 was at hand. Small limits (below) |

Beyond x86, Linux on s390x (big-endian), armhf, arm64, ppc64el, riscv64
and i386 passes the whole suite: cross-built with Debian 13's toolchains
and run under QEMU's user-mode emulation. So do the 32-bit big-endian
m68k, powerpc and hppa, cross-built with Debian unstable's (gcc 16). OpenBSD 7.9 on SPARC64 (64-bit
big-endian, gcc 4.2.1) builds and passes the twelve checks that need no
pseudo-terminal; the TUI could not be run there.

Everything but SuSE 8.1, Haiku, NetBSD 10.1, openSUSE and Windows 11 ran
in QEMU or, for Debian 2.1 and 2.2, in Docker; macOS and Cygwin run on
GitHub Actions (`.github/workflows/build.yml`), batch mode and the suite
only. The oldest Linux and BSD systems are compiled by hand.
[docs/BUILD.md](docs/BUILD.md) has the instructions for every system
here, [docs/TODO-OSES.md](docs/TODO-OSES.md) the ones still to try, and
[docs/TODO-REAL.md](docs/TODO-REAL.md) the ones that have only met an
emulator and want a run on real hardware.

## Packages

Releases carry ready-made packages: a `.deb` and an `.rpm`, static Linux
binaries for ten CPUs (`gork-<version>-linux-<arch>.tar.gz`: amd64, i386,
arm64, armhf, riscv64, s390x, ppc64el, powerpc, m68k, hppa), a macOS
installer, and the DOS and Windows zips below. Everything else builds from
the source tarball.

There are two DOS packages. `gork-<version>-dos.zip` needs a 386 and is
the full program. `gork-<version>-dos16.zip` runs in real mode, in 640K,
on anything down to an 8086, and where no protected mode is to be had
(Windows/386 2.x); there a conversation is kept to about 8000 tokens,
compacted from 4000, and a tool returns at most 8K at a time.

There are two Windows packages:

| Package | For | MCP servers | Commands run by |
|---|---|---|---|
| `gork-<version>-win32.zip` | 64-bit Windows 10 and later. The Cygwin build, with the Cygwin DLLs it needs; start `gork.cmd` | yes | a POSIX shell, `bin\sh.exe` in the package |
| `gork-<version>-win32-legacy.zip` | Windows 95 and NT 3.51 up to Windows 11: one small 32-bit `gork.exe`. `gorknt3.exe` beside it is the same for NT 3.1 and 3.5 | **no**: it cannot start MCP servers, and ignores `mcp.json` | `cmd.exe` (`COMMAND.COM` on Windows 9x) |

## Contents

- [Tested systems](#tested-systems)
- [Packages](#packages)
- [Why a relay](#why-a-relay)
- [Build](#build)
- [Run](#run)
  - [Interactive](#interactive)
  - [Local models](#local-models)
  - [Without TCP/IP: SPX](#without-tcpip-spx)
  - [Config](#config)
  - [Rules](#rules)
  - [Skills](#skills)
  - [MCP servers](#mcp-servers)
  - [Tools](#tools)
- [Known gaps](#known-gaps)
- [Working on gork](#working-on-gork)
- [Licence](#licence)

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
automake):

```sh
autoreconf -i
./configure
make
```

Produces `./gork`; `make install` puts it in `/usr/local/bin` (change with
`./configure --prefix=...`). cJSON is vendored in [cJSON/](cJSON/); the only
link dependencies are curses and `-lm`. On a UTF-8 terminal, configure with
`./configure LIBS=-lncursesw` so non-ASCII text renders.

[docs/BUILD.md](docs/BUILD.md) has the instructions for each operating
system: packages to install, flags, the one-line compiler command for
systems too old to run `configure`, and what to do without curses headers.

## Run

Start a relay on a machine that can do TLS:

```sh
socat TCP-LISTEN:8443,fork,reuseaddr OPENSSL:api.anthropic.com:443
```

or [gork-relay](https://github.com/yeold/gork-relay), which does the same
(`gork-relay api.anthropic.com`) and can also be reached
[without TCP/IP](#without-tcpip-spx), or, if you'd rather have a
long-lived service, stunnel in client mode:

```ini
[anthropic]
client  = yes
accept  = 0.0.0.0:8443
connect = api.anthropic.com:443
```

Then point gork at it in `~/.gork.conf` (`gork.cnf` on DOS and Windows:
see [Config](#config)):

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
`./gork --version` prints the version and exits.

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
`/v1/messages`.

For a server that only speaks the **OpenAI** chat completions format
(llama.cpp, Ollama, vLLM, OpenAI itself and the many services shaped like
it), add `protocol = openai`. It is confirmed working, tool calls
included, with GLM-5.3-Flash; it has not been tried from the DOS build.

```ini
relay    = localhost:11434
host     = localhost:11434
protocol = openai
model    = qwen3
```

gork then posts to `/v1/chat/completions` (change it with `path`) and sends
`api_key`, if there is one, as `Authorization: Bearer`. The key's
environment override becomes `OPENAI_API_KEY`, so that an exported
Anthropic key is never sent to another service. Everything else works the
same: tools, skills, MCP servers and compacting. What differs:

- No token limit is sent; the server's default applies.
- Effort goes out as `reasoning_effort`, with the same five names. Servers
  that know fewer levels may refuse `xhigh` or `max`.
- Replies are not streamed, and reasoning text a server returns beside the
  answer is dropped.
- A hosted service is HTTPS, so it needs a relay that forwards to it, as
  for Anthropic.

### Without TCP/IP: SPX

On an IPX network without TCP/IP, the DOS and Windows builds can reach a
[gork-relay](https://github.com/yeold/gork-relay) over SPX. The relay
prints its address when started with `-x`; give it to gork, in hex:

```ini
relay = spx:0.525400A1B2C3:8443
```

DOS, either build, needs an IPX driver with SPX loaded (`IPXODI`,
`IPX.COM`, or `PDIPX` on a packet driver); Windows 95 to XP need the IPX/SPX-compatible
protocol installed. Tested with PDIPX on FreeDOS and on Windows Me. The
Unix builds have no SPX.

### Config

gork reads `~/.gork.conf`, or the file `GORK_CONFIG` names. One
`key = value` per line; lines starting with `#` are comments. An unknown key
or a malformed line stops gork with the line number rather than being
skipped.

The DOS and Windows builds use other names, as a name with a leading dot
is awkward or impossible to create there. Wherever this README says
`~/.gork.conf`, `~/.gork` or `.gork`, read:

| | Unix | DOS | Windows |
|---|---|---|---|
| Config file | `~/.gork.conf` | `gork.cnf` beside `gork.exe` | `%USERPROFILE%\gork.cnf` (Windows 9x: beside `gork.exe`) |
| gork's directory (`trusted`, `skills`) | `~/.gork` | `_gork` beside `gork.exe` | `%USERPROFILE%\_gork` (Windows 9x: beside `gork.exe`) |
| Project skills, in the working directory | `.gork/skills` | `_gork\skills` | `_gork\skills` |

If `HOME` is set, DOS and Windows look there instead (it is, for one, in
Git Bash). gork's own messages name the full path of the file it means.
Neither build checks who can read the config file, and neither has MCP
servers, so there is no `mcp.json`.

| Key | Env override | Default | Meaning |
|---|---|---|---|
| `relay` | -- | *(required)* | `host:port` gork connects to: the relay, or a local server. Or `spx:NET.NODE:SOCKET`, a gork-relay on an IPX network: see [Without TCP/IP](#without-tcpip-spx) |
| `api_key` | `ANTHROPIC_API_KEY` (`OPENAI_API_KEY` with `protocol = openai`) | *(required for anthropic.com and openai.com)* | Sent as the `x-api-key` header, or as `Authorization: Bearer` with `protocol = openai` |
| `protocol` | `AGENT_PROTOCOL` | `anthropic` | Wire format: `anthropic` (Messages API) or `openai` (chat completions); see [Local models](#local-models) |
| `model` | `AGENT_MODEL` | `claude-opus-5` | Model id |
| `host` | `AGENT_HOST` | `api.anthropic.com` | `Host:` header value |
| `path` | `AGENT_PATH` | `/v1/messages`, or `/v1/chat/completions` with `protocol = openai` | Request path |
| `allow` | -- | *(none)* | Extra directories the file tools may read and write, `:`-separated (`;` on DOS and Windows; up to 8) |
| `max_turns` | `AGENT_MAX_TURNS` | `24` | Tool round-trips per task before gork gives up |
| `retries` | `AGENT_RETRIES` | `2` | Extra attempts for a request that hits a network error, 429 or 5xx (0..10) |
| `context_window` | `AGENT_CONTEXT_WINDOW` | `200000` | Tokens the model can take; set it to the loaded context length for a local model |
| `auto_compact` | `AGENT_AUTO_COMPACT` | `80` | Compact once the context passes this percentage of `context_window` (0 = never, max 99) |
| `timeout` | `AGENT_TIMEOUT` | `120` | Seconds one API request, MCP tool call or `run_command` may take before it fails |
| `default_effort` | `AGENT_DEFAULT_EFFORT` | *(unset: model default)* | Reasoning effort at startup: `low`, `medium`, `high`, `xhigh` or `max`, sent as `output_config.effort` (`reasoning_effort` with `protocol = openai`); `/effort` changes it in the TUI |

A set environment variable beats the file, and gork says so at startup (e.g. `model meta/muse-glimmer from AGENT_MODEL overrides ~/.gork.conf`), since a stale `export` in an old shell is easy to forget. If the file holds `api_key` and
anyone but you can read it, gork refuses to start: `chmod 600 ~/.gork.conf`.
(Not on DOS and Windows, the Cygwin build included: there are no such
modes there.)

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
from `~/.gork/skills/<name>/` (`_gork` on DOS and Windows: see
[Config](#config)); a project skill wins over a user skill of the
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

## Known gaps

- **Esc can't stop a running MCP call.** It waits for the call to finish
  (or hit `timeout`), then aborts.
- **Basic line editor.** No cursor movement, history, or multi-line input.
- **The TUI is only partly covered by the test suites.** They drive the
  trust prompt, approvals, Esc and `/quit`; the rest was checked by hand.
- **Path confinement is check-then-open.** A symlink swapped in between can
  escape it. That stops a model, not a hostile local user.
- **No streaming.** Replies are read to EOF, so a long generation is a long
  silence. Fine for a batch harness, poor for interactive use -- and more
  noticeable against a slow local model than against the API.
- **No chunked decoding.** gork asks for HTTP/1.0 so replies come back
  unchunked. A server that chunks anyway is detected and reported by name
  rather than failing as a JSON error; decoding it would be ~30 lines in
  `relay.c`.

## Working on gork

[docs/HACKING.md](docs/HACKING.md) has the layout of the source, how the
loop works, how to add a tool and how to run the tests;
[CONTRIBUTING.md](CONTRIBUTING.md) says how to send reports and changes.

## Licence

gork is free software: you can redistribute it and modify it under the
terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version. It comes with no warranty. The full text is in [LICENSE](LICENSE).

The vendored [cJSON](cJSON/) is under the MIT licence, which is at the top
of its files.
