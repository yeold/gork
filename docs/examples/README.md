# Example configs

| File | For | Copy to |
|---|---|---|
| [`.gork.conf`](.gork.conf) | Unix, macOS, the Cygwin package (`win32`) | `~/.gork.conf`; Cygwin: `%USERPROFILE%\.gork.conf` |
| [`gork.cnf`](gork.cnf) | DOS, and the `win32-legacy` `gork.exe` | DOS and Windows 9x: beside `gork.exe`; NT and later: `%USERPROFILE%\gork.cnf` |
| [`mcp.json`](mcp.json) | the builds with MCP servers: Unix, macOS, Cygwin | `~/.gork/mcp.json`; Cygwin: `%USERPROFILE%\.gork\mcp.json` |

The keys are in the README's [Config](../../README.md#config) and
[MCP servers](../../README.md#mcp-servers) sections.

In the config files a comment is a line of its own: text after a value,
`#` included, is part of the value.

`mcp.json` uses the Claude Code and Cline layout: `command`, `args` and
`env` sit on the server's entry (Zed's, which wraps them in `"transport"`,
is read too). gork starts `command` itself, without a shell, so it is one
program; its arguments go in `args`. A streamable HTTP server has a `url`
instead, as the `web` entry does; an `https://` one needs a `"relay"` that
speaks TLS (see the README's MCP section).

On the Cygwin package `command` is looked up by Cygwin, which may not run
Windows `.cmd` shims such as `npx.cmd`. If such a server does not start,
use `"command": "cmd.exe", "args": ["/c", "npx", ...]`.
