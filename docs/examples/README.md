# Examples

Copy a file, fill in the `REPLACE-ME`s, and `chmod 600` it if it holds a
key. The keys are in the README's [Config](../../README.md#config) and
[MCP servers](../../README.md#mcp-servers) sections.

## General configs

| File | For | Copy to |
|---|---|---|
| [`.gork.conf`](.gork.conf) | Unix, macOS, the Cygwin package (`win32`) | `~/.gork.conf`; Cygwin: `%USERPROFILE%\.gork.conf` |
| [`gork.cnf`](gork.cnf) | DOS, and the `win32-legacy` `gork.exe` | DOS and Windows 9x: beside `gork.exe`; NT and later: `%USERPROFILE%\gork.cnf` |

## One per service or setup

These are written as `~/.gork.conf`. On DOS and Windows, save the same
lines as `gork.cnf`.

| File | For |
|---|---|
| [`anthropic-tls.conf`](anthropic-tls.conf) | Claude on Anthropic's API, from a build `--with-openssl` (no relay) |
| [`openai.conf`](openai.conf) | OpenAI |
| [`openrouter.conf`](openrouter.conf) | OpenRouter |
| [`mistral.conf`](mistral.conf) | Mistral |
| [`lmstudio.conf`](lmstudio.conf) | LM Studio on this machine (Anthropic format) |
| [`ollama.conf`](ollama.conf) | Ollama on this machine |
| [`llamacpp.conf`](llamacpp.conf) | llama.cpp's `llama-server` on this machine |
| [`self-signed.conf`](self-signed.conf) | A server with a self-signed certificate (`ca_file`, `tls_verify`) |
| [`serial.conf`](serial.conf) | A Unix build `--with-serial`, over a null-modem cable |
| [`gork-nonet.cnf`](gork-nonet.cnf) | The DOS, Windows and UEFI `-nonet` packages, over a serial port |

The hosted services are HTTPS. The examples assume a build
`--with-openssl`; otherwise run [gork-relay](https://github.com/yeold/gork-relay)
for the service and add `relay = HOST:PORT`, keeping `host` as the service's name.

In the config files a comment is a line of its own: text after a value,
`#` included, is part of the value.

## Rules, skills and MCP servers

| File | What | Copy to |
|---|---|---|
| [`GORK.md`](GORK.md) | Project rules, sent as the system prompt | the project's directory |
| [`skills/commit/SKILL.md`](skills/commit/SKILL.md) | A skill | `.gork/skills/commit/` in the project, or `~/.gork/skills/commit/` (`_gork` on DOS and Windows) |
| [`mcp.json`](mcp.json) | MCP servers: Unix, macOS, Cygwin | `~/.gork/mcp.json`; Cygwin: `%USERPROFILE%\.gork\mcp.json` |

`mcp.json` uses the Claude Code and Cline layout: `command`, `args` and
`env` sit on the server's entry (Zed's, which wraps them in `"transport"`,
is read too). gork starts `command` itself, without a shell, so it is one
program; its arguments go in `args`. A streamable HTTP server has a `url`
instead, as the `web` entry does; an `https://` one needs a `"relay"` that
speaks TLS, unless gork is built `--with-openssl` (see the README's MCP
section).

On the Cygwin package `command` is looked up by Cygwin, which may not run
Windows `.cmd` shims such as `npx.cmd`. If such a server does not start,
use `"command": "cmd.exe", "args": ["/c", "npx", ...]`.
