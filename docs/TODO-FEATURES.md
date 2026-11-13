# TODO: features to add

Planned features and build options. No implementation yet.

All items previously listed here (`--with-max-file-size`, `--enable-unicode` with
autodetection and ASCII fallback, `--without-tui`, `--without-mcp`, session
save/resume via `/save` and `/load`, and showing the target path and file name on
file operations) have been implemented.

## Agent Client Protocol (ACP): gork as an agent an editor can drive

ACP is Zed's protocol for editors and agents: JSON-RPC 2.0 over the agent's
stdin/stdout, one message per line, nothing else on stdout (stderr is for logs).
gork would be the **agent** side, so an editor (Zed, or any ACP client) gets a chat
pane, streamed text, tool-call cards and approval prompts without a terminal.

Spec: <https://agentclientprotocol.com/protocol/v1/> — protocolVersion `1`. What a
v1 agent must answer:

- `initialize` → `protocolVersion`, `agentCapabilities` (`loadSession`,
  `promptCapabilities`, `mcpCapabilities`), `agentInfo` (`name`, `title`,
  `version`) and `authMethods` (empty: gork's key is the relay's or its config's).
- `session/new` (`cwd`, `mcpServers`) → `sessionId`. One ACP session is one gork
  conversation (`cJSON *messages`), and `cwd` is what the file tools are confined
  to, so it replaces `work_root` rather than adding to it.
- `session/prompt` (`sessionId`, `prompt` content blocks) → runs `run_task` on the
  text of the prompt, then answers `{ "stopReason": ... }`: `end_turn`,
  `max_tokens`, `max_turn_requests` (`max_turns` ran out), `refusal`, `cancelled`.
- Notifications `session/update` with `sessionUpdate`: `agent_message_chunk`
  (gork already streams text through `stream_text()` in `main.c` — that is the
  hook), `agent_thought_chunk` for thinking blocks, `tool_call` and
  `tool_call_update` (`pending`/`in_progress`/`completed`/`failed`, with `kind`
  from the tool: `read`, `edit`, `execute`, `other`) from `run_tools()`, and
  `usage_update` from `ctx_tokens`.
- Approval: `session/request_permission` with `allow_once`/`allow_always`/
  `reject_once` options instead of `tui_confirm()`. ACP's `allow_always` maps to
  the existing `always_ok[]` and the `trusted` file; a `cancelled` outcome (or
  `session/cancel`) sets `aborted`, which `relay_abort` already polls.
- `session/cancel` must be read **while** a prompt runs, so the stdin reader has
  to run alongside the model loop: `tui_poll_abort()` is that seam already.

Effort: one new file (`acp.c`, plus a `--acp` flag and its branch in `main()`),
framed like `mcp.c`, whose newline-delimited JSON-RPC reader (`struct buf rx` and
the line loop) is the pattern to copy rather than invent. The tool loop, tools,
config, confinement and streaming need no change; the front end is what is new.
Not for `GORK_NO_FORK`, `GORK_BARE`, `--with-serial` or the firmware builds: gork
is the subprocess here. Needs a `testing/fakeacp.py` client, as `fakemcp.py` is
for MCP, driving initialize → session/new → prompt → a tool call with a permission
request → cancel; `docs/MAINTAINING.md` and the README get their rows with it.

## Agent2Agent (A2A): gork as a remote agent other agents delegate to

A2A is the Linux Foundation protocol for one agent calling another over HTTP.
gork would be the **server**: publish an Agent Card, accept `message/send`, run
`run_task`, hand back the answer, and let the caller poll `tasks/get` or cancel
with `tasks/cancel`.

Spec: <https://a2a-protocol.org/latest/specification/> — v1.0.0, JSON-RPC binding
(`application/json`, `A2A-Version` header). Operation names per binding are its
section 5.3; in 0.3 the JSON-RPC ones were `message/send`, `message/stream`,
`tasks/get`, `tasks/list`, `tasks/cancel` and the push-notification configs, which
is what a client built today will send, so read 5.3 before writing the dispatch.
The Agent Card is served at
`/.well-known/agent-card.json` (`name`, `description`, `version`, `skills`,
`capabilities`, `securitySchemes`, and the JSON-RPC `supportedInterfaces`).
`Message` has a `role` and `parts[]` of `text`, `file` (uri or bytes) or `data`;
a `Task` has `id`, `contextId`, `status` (`submitted`, `working`,
`input-required`, `completed`, `failed`, `canceled`, `rejected`) and `artifacts`.

Mapping: one A2A task is one gork conversation started from the message's `text`
parts — `file` parts arrive as a path the model is told to `read_file`, and
`data` parts are dropped (say so in the card's `capabilities`) — `contextId`
groups tasks onto one gork conversation, and `artifacts` carry the final answer as
a single text part. `input-required` is the shape a `tui_confirm()` approval would
take without a terminal, so it is either `input-required` with a follow-up message,
or (lazier, and better for a headless server) a `--a2a-trust` mode that approves
writes and commands the way a trusted directory does.

Effort: the expensive one, because **gork has no listener anywhere today**:
`relay.c` only connects out. It needs `socket`/`bind`/`listen`/`accept`, an
HTTP/1.0 request read (the response side already exists in `relay.c`), a task
table keyed by `id`, and the JSON-RPC dispatch — a new `a2a.c` of its own, behind
a `--a2a PORT` flag, and it is the first gork code path that accepts a connection,
so its own confinement rule (the model may be steered by a remote agent: refuse
without a `cwd` or an `allow` list) and its own auth (compare a bearer token from
the config) matter more here than anywhere else. `PK` builds only: no DOS, no
firmware, no serial. A card and a `message/send` round trip against
`testing/fakea2a.py` (a client, per the spec's worked examples) are the check;
`message/stream` (SSE) and push notifications are a second step and can wait.
