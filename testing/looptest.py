"""Checks for the gork agent loop, run against fake Messages API servers.

Usage:  python3 testing/looptest.py ./gork
Exits 0 only if every check passes.  Fails loudly on anything else.

With $GORK_RELAY naming a gork-relay binary, every fake API server sits
behind it: the fake listens on port + 1000 and `gork-relay -p` on the
port gork is given.  Checks that need a TUI or MCP are skipped, and say
so, when the build has none (--without-tui, --without-mcp).
"""
import atexit, http.server, json, os, pty, re, select, socket, subprocess, sys, tempfile, threading, time

BINARY = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "./gork")
RELAY = os.environ.get("GORK_RELAY")


class Served:
    """A fake server, and the gork-relay in front of it if there is one."""

    def __init__(self, srv, relay=None):
        self.srv, self.relay = srv, relay

    def shutdown(self):
        if self.relay is not None:
            self.relay.kill()
            self.relay.wait()
        self.srv.shutdown()
        self.srv.server_close()


def serve(handler_cls, port, threaded=False):
    """Run handler_cls on port until the returned shutdown() is called."""
    cls = http.server.ThreadingHTTPServer if threaded else http.server.HTTPServer
    srv = cls(("127.0.0.1", port + 1000 if RELAY else port), handler_cls)
    srv.daemon_threads = True       # a hung request must not block the others
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    if not RELAY:
        return Served(srv)
    log = tempfile.TemporaryFile()
    relay = subprocess.Popen([RELAY, "-p", "-v", "-t", "127.0.0.1:%d" % port,
                              "127.0.0.1:%d" % (port + 1000)],
                             stdout=subprocess.DEVNULL, stderr=log)
    end = time.time() + 10
    while True:
        log.seek(0)
        if b"TCP on" in log.read():
            return Served(srv, relay)
        if relay.poll() is not None or time.time() > end:
            log.seek(0)
            srv.shutdown()
            raise AssertionError("gork-relay did not start on %d: %r"
                                 % (port, log.read()[-500:]))
        time.sleep(0.05)


def run_gork(port, task, cwd=None, config=None, mode=0o600, **env_extra):
    """Run gork on `task` with a config pointing at port (or `config`)."""
    env = dict(os.environ, ANTHROPIC_API_KEY="sk-test", AGENT_HOST="fake.local",
               HOME="/nonexistent")             # keep ~/.gork/skills out
    for k in ("GORK_ALLOW_RUN", "AGENT_MODEL", "AGENT_PATH"):
        env.pop(k, None)
    env.update(env_extra)
    env = {k: v for k, v in env.items() if v is not None}
    with tempfile.NamedTemporaryFile("w", suffix=".conf") as conf:
        conf.write(config if config is not None else f"relay = 127.0.0.1:{port}\n")
        conf.flush()
        os.chmod(conf.name, mode)
        env["GORK_CONFIG"] = conf.name
        try:
            return subprocess.run([BINARY, task], cwd=cwd, capture_output=True,
                                  text=True, timeout=30, env=env)
        except subprocess.TimeoutExpired as e:
            raise AssertionError("gork ran past 30 s; it said: %r"
                                 % (e.stderr or b"")[-2000:]) from None


def reply(handler, obj, status=200):
    body = json.dumps(obj).encode()
    handler.send_response(status)
    handler.send_header("content-type", "application/json")
    handler.send_header("content-length", str(len(body)))
    handler.end_headers()
    try:
        handler.wfile.write(body)
    except (BrokenPipeError, ConnectionResetError):
        pass                    # gork gave up first: the timeout checks


# ---------------------------------------------------------------- tool loop

def check_tool_loop():
    """One full tool_use round-trip: does gork ask, run, and feed back right?"""
    turns = []

    class H(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"

        def do_POST(self):
            req = json.loads(self.rfile.read(int(self.headers["content-length"])))
            turns.append(({k.lower(): v for k, v in self.headers.items()}, req))
            if len(turns) == 1:
                reply(self, {"id": "msg_01", "stop_reason": "tool_use", "content": [
                    {"type": "text", "text": "Reading it."},
                    {"type": "tool_use", "id": "toolu_01", "name": "read_file",
                     "input": {"path": "testing/req.json"}}]})
            else:
                reply(self, {"id": "msg_02", "stop_reason": "end_turn",
                             "content": [{"type": "text", "text": "done"}]})

        def log_message(self, *a):
            pass

    srv = serve(H, 8098)
    try:
        proc = run_gork(8098, "what's in req.json?")
    finally:
        srv.shutdown()

    assert proc.returncode == 0, f"exit {proc.returncode}: {proc.stderr}"
    assert "done" in proc.stdout, f"final text missing: {proc.stdout!r}"
    assert len(turns) == 2, f"expected 2 requests, got {len(turns)}"

    hdrs, first = turns[0]
    assert hdrs["x-api-key"] == "sk-test", hdrs.get("x-api-key")
    assert hdrs["anthropic-version"] == "2023-06-01", hdrs.get("anthropic-version")
    assert hdrs["host"] == "fake.local", hdrs.get("host")
    names = [t["name"] for t in first["tools"]]
    assert names == ["list_dir", "read_file", "write_file", "edit_file"], \
        f"run_command opt-in, no skill tool: {names}"
    assert first["messages"] == [{"role": "user", "content": "what's in req.json?"}]
    assert "thinking" not in first, "thinking must stay at its default"

    _, second = turns[1]
    msgs = second["messages"]
    assert len(msgs) == 3, msgs
    # assistant turn echoed back verbatim, tool_use block included
    assert msgs[1]["role"] == "assistant"
    assert [b["type"] for b in msgs[1]["content"]] == ["text", "tool_use"], msgs[1]
    # every result in ONE user turn, matched to its tool_use id
    assert msgs[2]["role"] == "user"
    res = msgs[2]["content"]
    assert len(res) == 1, res
    assert res[0]["type"] == "tool_result", res[0]
    assert res[0]["tool_use_id"] == "toolu_01", res[0]
    assert "is_error" not in res[0], "successful read flagged as an error"
    assert "claude-sonnet-5" in res[0]["content"], "file contents not returned"

    print("ok: tool_use round-trip")


def check_openai():
    """protocol = openai: the same round-trip as chat completions."""
    turns = []

    class H(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"

        def do_POST(self):
            req = json.loads(self.rfile.read(int(self.headers["content-length"])))
            turns.append(({k.lower(): v for k, v in self.headers.items()},
                          self.path, req))
            if len(turns) == 1:
                # "stop" with tool calls, as some local servers send it
                reply(self, {"id": "c1", "choices": [{"index": 0, "finish_reason": "stop",
                    "message": {"role": "assistant", "content": "Reading it.",
                                "tool_calls": [
                        {"id": "call_a", "type": "function", "function": {
                            "name": "read_file",
                            "arguments": "{\"path\": \"testing/req.json\"}"}},
                        {"id": "call_b", "type": "function", "function": {
                            "name": "read_file", "arguments": "not json"}}]}}],
                    "usage": {"prompt_tokens": 900, "completion_tokens": 100}})
            else:
                reply(self, {"id": "c2", "choices": [{"index": 0, "finish_reason": "stop",
                    "message": {"role": "assistant", "content": "done"}}]})

        def log_message(self, *a):
            pass

    conf = ("relay = 127.0.0.1:8091\nprotocol = openai\napi_key = sk-oa\n"
            "model = some-model\ndefault_effort = low\n")
    with tempfile.TemporaryDirectory() as tmp:
        with open(os.path.join(tmp, "GORK.md"), "w") as f:
            f.write("Be brief.\n")
        os.mkdir(os.path.join(tmp, "testing"))
        with open(os.path.join(tmp, "testing", "req.json"), "w") as f:
            f.write("the file\n")
        srv = serve(H, 8091)
        try:
            proc = run_gork(8091, "what's in req.json?", cwd=tmp, config=conf)
        finally:
            srv.shutdown()

    assert proc.returncode == 0, f"exit {proc.returncode}: {proc.stderr}"
    assert "Reading it." in proc.stdout and "done" in proc.stdout, proc.stdout
    assert len(turns) == 2, f"expected 2 requests, got {len(turns)}"

    hdrs, path, first = turns[0]
    assert path == "/v1/chat/completions", path
    assert hdrs["authorization"] == "Bearer sk-oa", "the Anthropic key in " \
        f"the environment must not be sent: {hdrs.get('authorization')}"
    assert "x-api-key" not in hdrs and "anthropic-version" not in hdrs, hdrs
    assert first["model"] == "some-model" and first["reasoning_effort"] == "low", first
    assert "system" not in first and "max_tokens" not in first, first
    assert first["messages"][0]["role"] == "system", first["messages"]
    assert first["messages"][0]["content"].startswith("Be brief."), first["messages"]
    assert first["messages"][1:] == [{"role": "user", "content": "what's in req.json?"}]
    tool = first["tools"][1]
    assert tool["type"] == "function" and tool["function"]["name"] == "read_file", tool
    assert tool["function"]["parameters"]["type"] == "object", tool

    msgs = turns[1][2]["messages"]
    assert [m["role"] for m in msgs] == ["system", "user", "assistant", "tool", "tool"], msgs
    assert msgs[2]["content"] == "Reading it.", msgs[2]
    calls = msgs[2]["tool_calls"]
    assert [c["id"] for c in calls] == ["call_a", "call_b"], calls
    assert calls[0]["type"] == "function", calls[0]
    assert json.loads(calls[0]["function"]["arguments"]) == {"path": "testing/req.json"}
    assert msgs[3] == {"role": "tool", "content": "the file\n", "tool_call_id": "call_a"}, msgs[3]
    # arguments that are not JSON reach the tool as {}, which says what it lacks
    assert msgs[4]["tool_call_id"] == "call_b" and "path" in msgs[4]["content"], msgs[4]

    proc = run_gork(8091, "hi", config="relay = 127.0.0.1:8091\nprotocol = openai\n"
                    "host = api.openai.com\n", AGENT_HOST=None)
    assert proc.returncode == 2 and "OPENAI_API_KEY" in proc.stderr, proc.stderr
    proc = run_gork(8091, "hi", config="relay = 127.0.0.1:8091\nprotocol = grpc\n")
    assert proc.returncode == 2 and "protocol" in proc.stderr, proc.stderr

    print("ok: protocol = openai round-trip")


# ------------------------------------------- rules, write/run, confinement

def check_rules_and_tools():
    """GORK.md becomes the system prompt; write_file and run_command work
    inside the working directory; every path out of it is refused."""
    calls = [
        ("write_file", {"path": "out.txt", "content": "hello\n"}),
        ("run_command", {"command": "cat out.txt; exit 3"}),
        ("write_file", {"path": "../escape.txt", "content": "x"}),
        ("read_file", {"path": "/etc/passwd"}),
        ("read_file", {"path": "up/etc/passwd"}),      # symlink to /
    ]
    turns = []

    class H(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"

        def do_POST(self):
            turns.append(json.loads(self.rfile.read(int(self.headers["content-length"]))))
            if len(turns) == 1:
                reply(self, {"id": "m1", "stop_reason": "tool_use", "content": [
                    {"type": "tool_use", "id": f"t{i}", "name": n, "input": a}
                    for i, (n, a) in enumerate(calls)]})
            else:
                reply(self, {"id": "m2", "stop_reason": "end_turn",
                             "content": [{"type": "text", "text": "done"}]})

        def log_message(self, *a):
            pass

    with tempfile.TemporaryDirectory() as tmp:
        work = os.path.join(tmp, "work")
        os.mkdir(work)
        os.symlink("/", os.path.join(work, "up"))
        with open(os.path.join(work, "GORK.md"), "w") as f:
            f.write("Always answer in haiku.\n")

        srv = serve(H, 8093)
        try:
            proc = run_gork(8093, "go", cwd=work, GORK_ALLOW_RUN="1")
        finally:
            srv.shutdown()

        assert proc.returncode == 0, f"exit {proc.returncode}: {proc.stderr}"
        with open(os.path.join(work, "out.txt")) as f:
            assert f.read() == "hello\n"
        assert not os.path.exists(os.path.join(tmp, "escape.txt")), "escaped!"

    first = turns[0]
    assert first["system"] == "Always answer in haiku.\n", first.get("system")
    assert "run_command" in [t["name"] for t in first["tools"]]

    res = {r["tool_use_id"]: r for r in turns[1]["messages"][2]["content"]}
    assert len(res) == len(calls), res
    assert "is_error" not in res["t0"], res["t0"]
    assert "hello" in res["t1"]["content"] and "[exit 3]" in res["t1"]["content"], res["t1"]
    assert res["t1"].get("is_error"), "nonzero exit not flagged"
    for t in ("t2", "t3", "t4"):
        assert res[t].get("is_error") and "outside" in res[t]["content"], res[t]

    print("ok: rules, write/run, path confinement")


# ------------------------------------------------------- local, unauthed

def check_local_no_key():
    """Local servers (LM Studio et al) need no key, and no relay in front."""
    seen = []

    class H(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"

        def do_POST(self):
            req = json.loads(self.rfile.read(int(self.headers["content-length"])))
            seen.append(({k.lower(): v for k, v in self.headers.items()},
                         self.path, req))
            reply(self, {"id": "m", "stop_reason": "end_turn",
                         "content": [{"type": "text", "text": "local hello"}]})

        def log_message(self, *a):
            pass

    srv = serve(H, 8095)
    try:
        proc = run_gork(8095, "hi", ANTHROPIC_API_KEY=None,
                         AGENT_HOST="localhost:1234",
                         AGENT_MODEL="ibm/granite-4-micro")
    finally:
        srv.shutdown()

    assert proc.returncode == 0, f"exit {proc.returncode}: {proc.stderr}"
    assert "local hello" in proc.stdout, proc.stdout
    hdrs, path, req = seen[0]
    assert path == "/v1/messages", path
    assert hdrs["host"] == "localhost:1234", hdrs.get("host")
    assert req["model"] == "ibm/granite-4-micro", req["model"]

    print("ok: local server, no API key")


def check_key_still_required_for_anthropic():
    """...but a missing key against the real API is still caught up front."""
    proc = run_gork(8095, "hi", ANTHROPIC_API_KEY=None,
                     AGENT_HOST="api.anthropic.com")
    assert proc.returncode == 2, f"exit {proc.returncode}: {proc.stderr}"
    assert "no API key" in proc.stderr, proc.stderr

    print("ok: missing key still caught for api.anthropic.com")


# ------------------------------------------------------- allow, edit_file

def check_allow_and_edit():
    """`allow` directories are reachable and announced; edit_file replaces
    exactly one match, refuses zero or several, and keeps file modes."""
    calls = [
        ("write_file", {"path": "SHARED/new.txt", "content": "made\n"}),
        ("read_file", {"path": "SHARED/notes.txt"}),
        ("edit_file", {"path": "SHARED/notes.txt", "old_text": "alpha",
                       "new_text": "ALPHA"}),
        ("edit_file", {"path": "SHARED/notes.txt", "old_text": "beta",
                       "new_text": "x"}),
        ("edit_file", {"path": "SHARED/notes.txt", "old_text": "zeta",
                       "new_text": "x"}),
        ("edit_file", {"path": "run.sh", "old_text": "echo old",
                       "new_text": "echo new"}),
        ("write_file", {"path": "OTHER/no.txt", "content": "x"}),
    ]
    turns = []

    class H(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"

        def do_POST(self):
            turns.append(json.loads(self.rfile.read(int(self.headers["content-length"]))))
            if len(turns) == 1:
                fix = lambda v: v.replace("SHARED", shared).replace("OTHER", other)
                reply(self, {"id": "m1", "stop_reason": "tool_use", "content": [
                    {"type": "tool_use", "id": f"t{i}", "name": n,
                     "input": {k: fix(v) for k, v in a.items()}}
                    for i, (n, a) in enumerate(calls)]})
            else:
                reply(self, {"id": "m2", "stop_reason": "end_turn",
                             "content": [{"type": "text", "text": "done"}]})

        def log_message(self, *a):
            pass

    with tempfile.TemporaryDirectory() as tmp:
        tmp = os.path.realpath(tmp)
        work, shared, other = (os.path.join(tmp, d) for d in ("work", "shared", "other"))
        for d in (work, shared, other):
            os.mkdir(d)
        with open(os.path.join(shared, "notes.txt"), "w") as f:
            f.write("alpha beta beta\n")
        script = os.path.join(work, "run.sh")
        with open(script, "w") as f:
            f.write("#!/bin/sh\necho old\n")
        os.chmod(script, 0o755)

        srv = serve(H, 8089)
        try:
            proc = run_gork(8089, "go", cwd=work,
                             config=f"relay = 127.0.0.1:8089\nallow = {shared}\n")
            bad = run_gork(8089, "go", cwd=work,
                            config=f"relay = 127.0.0.1:8089\nallow = {tmp}/typo\n")
        finally:
            srv.shutdown()

        assert proc.returncode == 0, f"exit {proc.returncode}: {proc.stderr}"
        assert bad.returncode == 2 and "typo" in bad.stderr, bad.stderr
        with open(os.path.join(shared, "new.txt")) as f:
            assert f.read() == "made\n"
        with open(os.path.join(shared, "notes.txt")) as f:
            assert f.read() == "ALPHA beta beta\n", "only the unique edit lands"
        with open(script) as f:
            assert f.read() == "#!/bin/sh\necho new\n"
        assert os.stat(script).st_mode & 0o777 == 0o755, "edit lost the mode"
        assert not os.path.exists(os.path.join(other, "no.txt"))
        assert not [n for d in (work, shared) for n in os.listdir(d)
                    if n.endswith(".gork-tmp")], "temp file left behind"

    assert shared in turns[0]["system"], "model not told about allow dirs"
    assert "edit_file" in [t["name"] for t in turns[0]["tools"]]

    res = {r["tool_use_id"]: r for r in turns[1]["messages"][2]["content"]}
    for ok in ("t0", "t1", "t2", "t5"):
        assert "is_error" not in res[ok], res[ok]
    assert res["t1"]["content"] == "alpha beta beta\n", res["t1"]
    assert res["t3"].get("is_error") and "more than once" in res["t3"]["content"]
    assert res["t4"].get("is_error") and "not found" in res["t4"]["content"]
    assert res["t6"].get("is_error") and "outside" in res["t6"]["content"]

    print("ok: allow directories, edit_file")


# ------------------------------------------------------------ list_dir

def check_list_dir():
    """list_dir lists sorted, marks dirs/symlinks, recurses only into
    non-hidden real directories, and stays inside the sandbox."""
    calls = [
        ("list_dir", {}),
        ("list_dir", {"path": ".", "recursive": True}),
        ("list_dir", {"path": "/etc"}),
        ("list_dir", {"path": "a.txt"}),
    ]
    turns = []

    class H(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"

        def do_POST(self):
            turns.append(json.loads(self.rfile.read(int(self.headers["content-length"]))))
            if len(turns) == 1:
                reply(self, {"id": "m1", "stop_reason": "tool_use", "content": [
                    {"type": "tool_use", "id": f"t{i}", "name": n, "input": a}
                    for i, (n, a) in enumerate(calls)]})
            else:
                reply(self, {"id": "m2", "stop_reason": "end_turn",
                             "content": [{"type": "text", "text": "done"}]})

        def log_message(self, *a):
            pass

    with tempfile.TemporaryDirectory() as work:
        os.makedirs(os.path.join(work, "sub", "deep"))
        os.makedirs(os.path.join(work, ".hidden"))
        for p, body in (("a.txt", "12345"), ("sub/b.txt", "x"),
                        ("sub/deep/c.txt", ""), (".hidden/secret", "")):
            with open(os.path.join(work, p), "w") as f:
                f.write(body)
        os.symlink("/", os.path.join(work, "up"))

        srv = serve(H, 8086)
        try:
            proc = run_gork(8086, "go", cwd=work)
        finally:
            srv.shutdown()
    assert proc.returncode == 0, f"exit {proc.returncode}: {proc.stderr}"

    res = {r["tool_use_id"]: r for r in turns[1]["messages"][2]["content"]}
    assert res["t0"]["content"] == \
        ".hidden/\na.txt  (5 bytes)\nsub/\nup@\n", res["t0"]["content"]
    assert res["t1"]["content"] == (
        ".hidden/\na.txt  (5 bytes)\nsub/\nsub/b.txt  (1 bytes)\n"
        "sub/deep/\nsub/deep/c.txt  (0 bytes)\nup@\n"), res["t1"]["content"]
    assert res["t2"].get("is_error") and "outside" in res["t2"]["content"], res["t2"]
    assert res["t3"].get("is_error") and "not a directory" in res["t3"]["content"]

    print("ok: list_dir")


def reap(pid, fd):
    """Wait for gork to exit while still reading its pty.  On the way out it
    waits for its output to drain, and a BSD pty never drains into a master
    nobody reads (Linux does not block there)."""
    while True:
        done, status = os.waitpid(pid, os.WNOHANG)
        if done:
            return status
        if select.select([fd], [], [], 0.1)[0]:
            try:
                os.read(fd, 4096)
            except OSError:
                pass


def idle_after(out, marker):
    """Has the TUI gone back to its input prompt since it showed `marker`?
    Keys typed while a task is still running are dropped, so /quit must wait."""
    return marker in out and b"Enter send" in out[out.rindex(marker):]


# --------------------------------------------------------------- trust

def interact(cwd, home, keys):
    """Run interactive gork on a pty, answer the trust prompt with `keys`
    if it appears, quit the TUI if it starts.  Returns (exit code, output)."""
    pid, fd = pty.fork()
    if pid == 0:
        os.chdir(cwd)
        os.environ.update(HOME=home, TERM="xterm")
        os.environ.pop("ANTHROPIC_API_KEY", None)
        os.environ["AGENT_HOST"] = "fake.local"
        with open(os.path.join(home, "k.conf"), "w") as f:
            f.write("relay = 127.0.0.1:9\n")
        os.environ["GORK_CONFIG"] = os.path.join(home, "k.conf")
        os.execv(BINARY, [BINARY])

    out, sent_keys, sent_quit, deadline = b"", False, False, time.time() + 10
    while time.time() < deadline:
        r, _, _ = select.select([fd], [], [], 0.2)
        if r:
            try:
                chunk = os.read(fd, 4096)
            except OSError:
                break
            if not chunk:
                break
            out += chunk
        if b"Trust this directory?" in out and not sent_keys:
            os.write(fd, keys)
            sent_keys = True
        if b"Enter send" in out and not sent_quit:
            os.write(fd, b"/quit\r")
            sent_quit = True
    status = reap(pid, fd)
    return os.waitstatus_to_exitcode(status), out.decode(errors="replace")


def check_trust():
    """The TUI asks before trusting a new directory, quits on no, remembers
    yes (for subdirectories too), and never asks in batch mode."""
    with tempfile.TemporaryDirectory() as tmp:
        tmp = os.path.realpath(tmp)
        home, work = os.path.join(tmp, "home"), os.path.join(tmp, "work")
        os.makedirs(home)
        os.makedirs(os.path.join(work, "sub"))
        trusted = os.path.join(home, ".gork", "trusted")

        code, out = interact(work, home, b"n\n")
        assert code == 1 and "Trust this directory?" in out, (code, out)
        assert "Enter send" not in out, "TUI started after declining"
        assert not os.path.exists(trusted)

        code, out = interact(work, home, b"e\n")
        assert code == 0 and "Enter send" in out, (code, out)
        with open(trusted) as f:
            assert f.read() == f"edits {work}\n"

        code, out = interact(os.path.join(work, "sub"), home, b"n\n")
        assert code == 0 and "Trust this" not in out, "subdir should inherit"

        code, out = interact(home, home, b"n\n")
        assert "your home directory" in out, out

    print("ok: trust prompt")


# ----------------------------------------------------------------- mcp

FAKEMCP = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fakemcp.py")


FAKES = []


@atexit.register
def kill_fakes():
    """A fake server left running keeps the output pipe open, and a CI step
    that pipes the suite through tee would wait for it for ever."""
    for fake in FAKES:
        if fake.poll() is None:
            fake.kill()


def fakemcp_http(port, log):
    """fakemcp.py on streamable HTTP, once it takes connections."""
    fake = subprocess.Popen([sys.executable, FAKEMCP, "--http", str(port)],
                            env=dict(os.environ, FAKEMCP_LOG=log),
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    FAKES.append(fake)
    end = time.time() + 10
    while True:
        assert fake.poll() is None, "fakemcp.py --http %d died" % port
        try:
            socket.create_connection(("127.0.0.1", port), 1).close()
            return fake
        except OSError:
            assert time.time() < end, "fakemcp.py --http %d did not start" % port
            time.sleep(0.1)


def check_mcp():
    """Servers in ~/.gork/mcp.json are started, their tools offered as
    mcp__<server>__<tool>, and calls round-trip.  A broken or disabled
    server is skipped without taking the others down; an entry in Zed's
    layout, with a "transport", is read as well."""
    turns = []

    class H(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"

        def do_POST(self):
            turns.append(json.loads(self.rfile.read(int(self.headers["content-length"]))))
            if len(turns) == 1:
                reply(self, {"id": "m1", "stop_reason": "tool_use", "content": [
                    {"type": "tool_use", "id": "t0", "name": "mcp__fake__echo",
                     "input": {"text": "hi"}},
                    {"type": "tool_use", "id": "t1", "name": "mcp__fake__fail",
                     "input": {}}]})
            else:
                reply(self, {"id": "m2", "stop_reason": "end_turn",
                             "content": [{"type": "text", "text": "done"}]})

        def log_message(self, *a):
            pass

    with tempfile.TemporaryDirectory() as home:
        os.makedirs(os.path.join(home, ".gork"))
        with open(os.path.join(home, ".gork", "mcp.json"), "w") as f:
            json.dump({"mcpServers": {
                "broken": {"command": "/nonexistent/server"},
                "picky": {"command": sys.executable, "args": [FAKEMCP],
                          "tools": ["echo"]},
                "zed": {"transport": {"type": "stdio",
                                      "command": sys.executable,
                                      "args": [FAKEMCP]},
                        "tools": ["fail"]},
                "fake": {"command": sys.executable, "args": [FAKEMCP]},
                "off": {"command": sys.executable, "args": [FAKEMCP],
                        "disabled": True},
                "remote": {"url": "https://example.com/mcp"},
            }}, f)

        srv = serve(H, 8088)
        try:
            proc = run_gork(8088, "go", HOME=home)
        finally:
            srv.shutdown()

    assert proc.returncode == 0, f"exit {proc.returncode}: {proc.stderr}"
    assert "broken: initialize failed" in proc.stderr, proc.stderr
    assert "remote: gork has no TLS" in proc.stderr, proc.stderr

    names = [t["name"] for t in turns[0]["tools"]]
    assert names[-2:] == ["mcp__fake__echo", "mcp__fake__fail"], names
    assert not [n for n in names if n.startswith("mcp__off")], names
    assert [n for n in names if n.startswith("mcp__picky")] == \
        ["mcp__picky__echo"], names
    assert [n for n in names if n.startswith("mcp__zed")] == \
        ["mcp__zed__fail"], names
    echo = turns[0]["tools"][-2]
    assert echo["input_schema"]["required"] == ["text"], echo

    res = {r["tool_use_id"]: r for r in turns[1]["messages"][2]["content"]}
    assert res["t0"]["content"] == "echo: hi\n[image content omitted]\n", res["t0"]
    assert "is_error" not in res["t0"], res["t0"]
    assert res["t1"].get("is_error") and "it broke" in res["t1"]["content"], res["t1"]

    print("ok: mcp")


def check_mcp_http():
    """A streamable HTTP server: reached directly by http://, and by https://
    through a "relay"; its session id and headers go back with each request,
    events in a reply are taken apart, and the session is deleted at the
    end."""
    turns = []

    class H(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"

        def do_POST(self):
            turns.append(json.loads(self.rfile.read(int(self.headers["content-length"]))))
            if len(turns) == 1:
                reply(self, {"id": "m1", "stop_reason": "tool_use", "content": [
                    {"type": "tool_use", "id": "t0", "name": "mcp__web__echo",
                     "input": {"text": "hi"}},
                    {"type": "tool_use", "id": "t1", "name": "mcp__tls__fail",
                     "input": {}}]})
            else:
                reply(self, {"id": "m2", "stop_reason": "end_turn",
                             "content": [{"type": "text", "text": "done"}]})

        def log_message(self, *a):
            pass

    with tempfile.TemporaryDirectory() as home:
        log = os.path.join(home, "mcp.log")
        fake = fakemcp_http(8103, log)
        os.makedirs(os.path.join(home, ".gork"))
        with open(os.path.join(home, ".gork", "mcp.json"), "w") as f:
            json.dump({"mcpServers": {
                "web": {"url": "http://127.0.0.1:8103/mcp",
                        "headers": {"X-Token": "t1"}},
                "tls": {"url": "https://mcp.example/mcp",
                        "relay": "127.0.0.1:8103",
                        "headers": {"X-Token": "t1"}},
            }}, f)
        srv = serve(H, 8104)
        try:
            proc = run_gork(8104, "go", HOME=home)
        finally:
            srv.shutdown()
            fake.kill()
            fake.wait()
        notes = open(log).read() if os.path.exists(log) else ""

    assert proc.returncode == 0, f"exit {proc.returncode}: {proc.stderr}"
    names = [t["name"] for t in turns[0]["tools"]]
    for n in ("mcp__web__echo", "mcp__web__fail", "mcp__tls__echo",
              "mcp__tls__fail"):
        assert n in names, (names, proc.stderr)
    res = {r["tool_use_id"]: r for r in turns[1]["messages"][2]["content"]}
    assert res["t0"]["content"] == "echo: hi\n[image content omitted]\n", res["t0"]
    assert res["t1"].get("is_error") and "it broke" in res["t1"]["content"], res["t1"]
    assert "no session" not in notes, notes
    assert "answer p1" in notes, notes                  # the ping in the events
    assert "proto 2025-06-18 token t1" in notes, notes
    assert notes.count("deleted s-1") == 2, notes

    print("ok: mcp over streamable HTTP")


# -------------------------------------------------------------- skills

def check_skills():
    """Skills from the project and ~/.gork/skills are indexed in the system
    prompt, load through the skill tool, and the user's skill files are
    readable but not writable."""
    calls = [
        ("skill", {"name": "deploy"}),
        ("skill", {"name": "nope"}),
        ("read_file", {"path": "HOME/.gork/skills/deploy/ref.txt"}),
        ("write_file", {"path": "HOME/.gork/skills/deploy/x.txt", "content": "x"}),
    ]
    turns = []

    class H(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"

        def do_POST(self):
            turns.append(json.loads(self.rfile.read(int(self.headers["content-length"]))))
            if len(turns) == 1:
                reply(self, {"id": "m1", "stop_reason": "tool_use", "content": [
                    {"type": "tool_use", "id": f"t{i}", "name": n,
                     "input": {k: v.replace("HOME", home) for k, v in a.items()}}
                    for i, (n, a) in enumerate(calls)]})
            else:
                reply(self, {"id": "m2", "stop_reason": "end_turn",
                             "content": [{"type": "text", "text": "done"}]})

        def log_message(self, *a):
            pass

    def skill(root, name, front, body="Do the thing.\n"):
        os.makedirs(os.path.join(root, name))
        with open(os.path.join(root, name, "SKILL.md"), "w") as f:
            f.write(front + body)

    with tempfile.TemporaryDirectory() as tmp:
        tmp = os.path.realpath(tmp)
        home, work = os.path.join(tmp, "home"), os.path.join(tmp, "work")
        user, proj = (os.path.join(home, ".gork", "skills"),
                      os.path.join(work, ".gork", "skills"))
        skill(user, "deploy", "---\nname: deploy\ndescription: >\n  Ship the\n"
                              "  release.\n---\n", "Run make, then ref.txt.\n")
        with open(os.path.join(user, "deploy", "ref.txt"), "w") as f:
            f.write("reference notes\n")
        skill(user, "lint", "---\nname: lint\ndescription: user lint\n---\n")
        skill(proj, "lint", '---\nname: lint\ndescription: "project lint"\n---\n')
        skill(proj, "bare", "no frontmatter here\n")

        srv = serve(H, 8090)
        try:
            proc = run_gork(8090, "go", cwd=work, HOME=home)
        finally:
            srv.shutdown()

        assert proc.returncode == 0, f"exit {proc.returncode}: {proc.stderr}"
        assert not os.path.exists(os.path.join(user, "deploy", "x.txt"))

    system = turns[0]["system"]
    assert "- deploy: Ship the release." in system, system
    assert "- lint: project lint" in system, "project skill should win"
    assert "user lint" not in system and "bare" not in system, system
    assert "skill" in [t["name"] for t in turns[0]["tools"]]

    res = {r["tool_use_id"]: r for r in turns[1]["messages"][2]["content"]}
    assert "Run make" in res["t0"]["content"] and "is_error" not in res["t0"], res["t0"]
    assert "Skill directory: " + user in res["t0"]["content"], res["t0"]
    assert res["t1"].get("is_error"), res["t1"]
    assert res["t2"]["content"] == "reference notes\n", res["t2"]
    assert res["t3"].get("is_error") and "outside" in res["t3"]["content"], res["t3"]

    print("ok: skills")


# -------------------------------------------------------------- config

def check_config():
    """Settings come from the config file; the environment beats it; a bad
    file is refused with a reason rather than half-used."""
    seen, delay = [], [0]

    class H(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"

        def do_POST(self):
            req = json.loads(self.rfile.read(int(self.headers["content-length"])))
            time.sleep(delay[0])
            seen.append(({k.lower(): v for k, v in self.headers.items()},
                         self.path, req))
            reply(self, {"id": "m", "stop_reason": "end_turn",
                         "content": [{"type": "text", "text": "ok"}]})

        def log_message(self, *a):
            pass

    conf = ("# gork settings\n"
            "relay   = 127.0.0.1:8092\n"
            "  model = cfg-model  \n"
            "\n"
            "host=cfg.local\n"
            "path = /cfg/messages\n"
            "api_key = sk-cfg\n")
    srv = serve(H, 8092)
    try:
        proc = run_gork(8092, "hi", config=conf, ANTHROPIC_API_KEY=None,
                         AGENT_HOST=None, AGENT_MODEL="env-model")
        same = run_gork(8092, "hi", config=conf, ANTHROPIC_API_KEY="sk-env",
                         AGENT_HOST="cfg.local")    # same as the file: quiet
        effort = run_gork(8092, "hi", config=conf + "default_effort = xhigh\n")
        delay[0] = 2
        short = run_gork(8092, "hi", config=conf + "timeout = 1\nretries = 0\n")
        long_ = run_gork(8092, "hi", config=conf + "timeout = 5\n")
    finally:
        srv.shutdown()

    assert proc.returncode == 0, f"exit {proc.returncode}: {proc.stderr}"
    hdrs, path, req = seen[0]
    assert hdrs["host"] == "cfg.local", hdrs.get("host")
    assert hdrs["x-api-key"] == "sk-cfg", hdrs.get("x-api-key")
    assert path == "/cfg/messages", path
    assert req["model"] == "env-model", "environment must beat the file"
    assert "model env-model from AGENT_MODEL overrides" in proc.stderr, proc.stderr
    assert "(cfg-model)" in proc.stderr, proc.stderr
    assert "host" not in proc.stderr, "AGENT_HOST unset: nothing to report"

    assert same.returncode == 0, same.stderr
    assert "api_key from ANTHROPIC_API_KEY overrides" in same.stderr, same.stderr
    assert "sk-env" not in same.stderr and "sk-cfg" not in same.stderr, "key leaked"
    assert "host" not in same.stderr, same.stderr
    assert "output_config" not in seen[0][2], "effort unset: send nothing"

    assert effort.returncode == 0, effort.stderr
    assert seen[2][2]["output_config"] == {"effort": "xhigh"}, seen[2][2]
    assert short.returncode != 0 and "timeout" in short.stderr, short.stderr
    assert long_.returncode == 0, long_.stderr
    proc = run_gork(8092, "hi", config="relay = 127.0.0.1:8092\ntimeout = 0\n")
    assert proc.returncode == 2 and "want timeout" in proc.stderr, proc.stderr
    proc = run_gork(8092, "hi", config="relay = 127.0.0.1:8092\ndefault_effort = igh\n")
    assert proc.returncode == 2 and "want default_effort" in proc.stderr, proc.stderr

    if sys.platform != "cygwin":        # no such rule there: see main.c
        proc = run_gork(8092, "hi", config=conf, mode=0o644, ANTHROPIC_API_KEY=None)
        assert proc.returncode == 2 and "chmod 600" in proc.stderr, proc.stderr

    proc = run_gork(8092, "hi", config="relay = 127.0.0.1:8092\nrelya = x\n")
    assert proc.returncode == 2 and ":2:" in proc.stderr, proc.stderr

    proc = run_gork(8092, "hi", config="model = m\n")
    assert proc.returncode == 2 and "no relay" in proc.stderr, proc.stderr

    proc = run_gork(8092, "hi", config="relay = nohost\n")
    assert proc.returncode == 2 and "bad relay" in proc.stderr, proc.stderr

    print("ok: config file")


# ------------------------------------------------------------- chunked

def chunked(body, sizes=(1, 10, 3)):
    """`body` in HTTP chunks: the given sizes, then the rest, with a chunk
    extension and a trailer thrown in."""
    out, i = b"", 0
    for n in list(sizes) + [len(body)]:
        part, body = body[:n], body[n:]
        if not part:
            break
        out += b"%X%s\r\n%s\r\n" % (len(part), b";x=y" if i == 1 else b"", part)
        i += 1
    return out + b"0\r\nX-Trailer: 1\r\n\r\n"


def check_chunked():
    """A chunked reply is decoded, whatever the pieces it arrives in; one
    that breaks off in a chunk is an error, not half an answer."""
    body = json.dumps({"id": "m", "stop_reason": "end_turn",
                       "content": [{"type": "text", "text": "chunks are fine"}]}).encode()
    cut = [False]

    class H(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"

        def do_POST(self):
            self.rfile.read(int(self.headers["content-length"]))
            out = (b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                   b"Transfer-Encoding: chunked\r\n\r\n" + chunked(body))
            if cut[0]:
                out = out[:len(out) - 40]
            for i in range(0, len(out), 7):         # in small pieces
                self.wfile.write(out[i:i + 7])
                self.wfile.flush()

        def log_message(self, *a):
            pass

    srv = serve(H, 8094)
    try:
        proc = run_gork(8094, "hi")
        cut[0] = True
        short = run_gork(8094, "hi", config="relay = 127.0.0.1:8094\nretries = 0\n")
    finally:
        srv.shutdown()

    assert proc.returncode == 0 and proc.stdout == "chunks are fine\n", \
        (proc.stdout, proc.stderr)
    assert short.returncode != 0 and "chunk" in short.stderr, short.stderr
    assert "JSON" not in short.stderr, short.stderr

    print("ok: chunked reply decoded")


# ------------------------------------------------------------ streaming

def sse(handler, events, chunks=False):
    """Send `events` (dicts, or strings sent as they are) as an event
    stream, a few bytes at a time."""
    out = b""
    for ev in events:
        if isinstance(ev, str):
            out += ev.encode()
        else:
            out += b"event: %s\ndata: %s\n\n" % (ev.get("type", "x").encode(),
                                               json.dumps(ev).encode())
    head = b"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
    if chunks:
        out = chunked(out, sizes=(5, 40, 9, 200))
        head += b"Transfer-Encoding: chunked\r\n"
    handler.wfile.write(head + b"\r\n")
    for i in range(0, len(out), 11):
        handler.wfile.write(out[i:i + 11])
        handler.wfile.flush()


def text_events(text, stop="end_turn", pieces=3):
    """The events of a reply that is one block of text."""
    n = max(1, len(text) // pieces)
    parts = [text[i:i + n] for i in range(0, len(text), n)]
    return ([{"type": "message_start", "message": {
                  "id": "m", "role": "assistant", "content": [],
                  "usage": {"input_tokens": 900, "output_tokens": 1}}},
             {"type": "content_block_start", "index": 0,
              "content_block": {"type": "text", "text": ""}}, {"type": "ping"}] +
            [{"type": "content_block_delta", "index": 0,
              "delta": {"type": "text_delta", "text": p}} for p in parts] +
            [{"type": "content_block_stop", "index": 0},
             {"type": "message_delta", "delta": {"stop_reason": stop},
              "usage": {"output_tokens": 30}},
             {"type": "message_stop"}])


def check_stream():
    """`stream = yes`: the pieces of a streamed reply are printed and put
    back together into the message a plain reply would have been -- text,
    thinking with its signature, and a tool call whose JSON came in parts.
    The same through chunked transfer.  Batch mode does not ask for it."""
    turns = []
    want = [{"type": "thinking", "thinking": "Let me look.", "signature": "c2ln"},
            {"type": "text", "text": "Reading it—now."},
            {"type": "tool_use", "id": "toolu_9", "name": "read_file",
             "input": {"path": "testing/req.json"}}]
    first = [
        {"type": "message_start", "message": {
            "id": "m1", "role": "assistant", "content": [],
            "usage": {"input_tokens": 1200, "cache_read_input_tokens": 50,
                      "output_tokens": 1}}},
        {"type": "content_block_start", "index": 0,
         "content_block": {"type": "thinking", "thinking": "", "signature": ""}},
        {"type": "content_block_delta", "index": 0,
         "delta": {"type": "thinking_delta", "thinking": "Let me "}},
        {"type": "content_block_delta", "index": 0,
         "delta": {"type": "thinking_delta", "thinking": "look."}},
        {"type": "content_block_delta", "index": 0,
         "delta": {"type": "signature_delta", "signature": "c2ln"}},
        {"type": "content_block_stop", "index": 0},
        {"type": "content_block_start", "index": 1,
         "content_block": {"type": "text", "text": ""}},
        {"type": "ping"},
        {"type": "content_block_delta", "index": 1,
         "delta": {"type": "text_delta", "text": "Reading "}},
        ": a comment line\n\n",
        {"type": "content_block_delta", "index": 1,
         "delta": {"type": "text_delta", "text": "it—now."}},
        {"type": "content_block_stop", "index": 1},
        {"type": "content_block_start", "index": 2,
         "content_block": {"type": "tool_use", "id": "toolu_9",
                           "name": "read_file", "input": {}}},
        {"type": "content_block_delta", "index": 2,
         "delta": {"type": "input_json_delta", "partial_json": ""}},
        {"type": "content_block_delta", "index": 2,
         "delta": {"type": "input_json_delta", "partial_json": "{\"path\": \"test"}},
        {"type": "content_block_delta", "index": 2,
         "delta": {"type": "input_json_delta", "partial_json": "ing/req.json\"}"}},
        {"type": "content_block_stop", "index": 2},
        {"type": "message_delta", "delta": {"stop_reason": "tool_use"},
         "usage": {"output_tokens": 77}},
        {"type": "message_stop"}]

    class H(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"

        def do_POST(self):
            req = json.loads(self.rfile.read(int(self.headers["content-length"])))
            turns.append(req)
            n = len(turns)
            if not req.get("stream"):
                reply(self, {"id": "m", "stop_reason": "end_turn",
                             "content": [{"type": "text", "text": "not streamed"}]})
            elif n in (1, 3):
                sse(self, first, chunks=n == 3)
            elif n == 5:                # a server that takes no notice
                reply(self, {"id": "m", "stop_reason": "end_turn",
                             "content": [{"type": "text", "text": "plain after all"}]})
            elif n == 6:
                sse(self, text_events("half an ans")[:5] +
                    [{"type": "error", "error": {"type": "overloaded_error",
                                                 "message": "Overloaded"}}])
            elif n == 7:                # the connection drops in mid-reply
                sse(self, text_events("cut off here")[:5])
            else:
                sse(self, text_events("all done"), chunks=n == 4)

        def log_message(self, *a):
            pass

    conf = "relay = 127.0.0.1:8099\nstream = yes\nretries = 0\n"
    srv = serve(H, 8099)
    try:
        runs = [run_gork(8099, "go", config=conf) for _ in range(2)]
        plain = run_gork(8099, "go", config=conf)
        error = run_gork(8099, "go", config=conf)
        dropped = run_gork(8099, "go", config=conf)
        batch = run_gork(8099, "go")
        bad = run_gork(8099, "go", config="relay = 127.0.0.1:8099\nstream = maybe\n")
    finally:
        srv.shutdown()

    for k, proc in enumerate(runs):
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout == "Reading it—now.\nall done\n", proc.stdout
        a, b = turns[2 * k], turns[2 * k + 1]
        assert a["stream"] is True
        # the reply went back as it was, block for block
        assert b["messages"][1] == {"role": "assistant", "content": want}, \
            b["messages"][1]
        res = b["messages"][2]["content"]
        assert res[0]["tool_use_id"] == "toolu_9" and "claude-sonnet-5" in res[0]["content"]
    assert plain.stdout == "plain after all\n", (plain.stdout, plain.stderr)
    assert error.returncode != 0 and "Overloaded" in error.stderr, error.stderr
    assert error.stdout == "half a\n", error.stdout     # what had come
    assert dropped.returncode != 0 and "before its end" in dropped.stderr, dropped.stderr
    assert "stream" not in turns[7] and batch.stdout == "not streamed\n", batch.stdout
    assert bad.returncode == 2 and "want stream" in bad.stderr, bad.stderr

    print("ok: streamed replies, plain and chunked")


def check_stream_openai():
    """The same for `protocol = openai`: content and tool calls in deltas,
    the arguments in parts, the token counts in a last chunk."""
    turns = []

    def chunk(delta=None, finish=None, **more):
        d = {"id": "c", "object": "chat.completion.chunk",
             "choices": [] if delta is None and finish is None else
                        [{"index": 0, "delta": delta or {}, "finish_reason": finish}]}
        d.update(more)
        return "data: %s\n\n" % json.dumps(d)

    first = [chunk({"role": "assistant", "content": ""}),
             chunk({"content": "Look"}), chunk({"content": "ing."}),
             chunk({"tool_calls": [{"index": 0, "id": "call_7", "type": "function",
                                    "function": {"name": "read_file", "arguments": ""}}]}),
             chunk({"tool_calls": [{"index": 0, "function": {"arguments": "{\"path\":"}}]}),
             chunk({"tool_calls": [{"index": 0, "function": {"arguments": " \"testing/req.json\"}"}}]}),
             chunk(finish="tool_calls"),
             chunk(usage={"prompt_tokens": 800, "completion_tokens": 20}),
             "data: [DONE]\n\n"]
    second = [chunk({"role": "assistant", "content": "It is "}),
              chunk({"content": "a request."}), chunk(finish="stop"),
              "data: [DONE]\n\n"]

    class H(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"

        def do_POST(self):
            turns.append(json.loads(self.rfile.read(int(self.headers["content-length"]))))
            sse(self, first if len(turns) == 1 else second, chunks=len(turns) == 2)

        def log_message(self, *a):
            pass

    srv = serve(H, 8100)
    try:
        proc = run_gork(8100, "go", config="relay = 127.0.0.1:8100\n"
                        "protocol = openai\npath = /v1/chat/completions\n"
                        "stream = yes\n", ANTHROPIC_API_KEY=None)
    finally:
        srv.shutdown()

    assert proc.returncode == 0, proc.stderr
    assert proc.stdout == "Looking.\nIt is a request.\n", proc.stdout
    assert turns[0]["stream"] is True
    assert turns[0]["stream_options"] == {"include_usage": True}
    msgs = turns[1]["messages"]
    call = msgs[-2]["tool_calls"][0]
    assert msgs[-2]["content"] == "Looking." and call["id"] == "call_7"
    assert call["function"]["name"] == "read_file"
    assert json.loads(call["function"]["arguments"]) == {"path": "testing/req.json"}
    assert msgs[-1]["role"] == "tool" and msgs[-1]["tool_call_id"] == "call_7"

    print("ok: streamed replies, protocol = openai")


def check_tui_stream():
    """In the TUI a reply is asked for as a stream and shows as it comes:
    the first words are on the screen while the rest is still on its way."""
    turns, go_on = [], threading.Event()

    class H(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"

        def do_POST(self):
            turns.append(json.loads(self.rfile.read(int(self.headers["content-length"]))))
            ev = text_events("EARLY words, and the LATER ones", pieces=2)
            sse(self, ev[:4])
            go_on.wait(10)
            for e in ev[4:]:
                self.wfile.write(b"data: %s\n\n" % json.dumps(e).encode())
            self.wfile.flush()

        def log_message(self, *a):
            pass

    srv = serve(H, 8101, threaded=True)
    with tempfile.TemporaryDirectory() as tmp:
        tmp = os.path.realpath(tmp)
        os.makedirs(os.path.join(tmp, ".gork"))
        with open(os.path.join(tmp, ".gork", "trusted"), "w") as f:
            f.write(f"trust {tmp}\n")
        with open(os.path.join(tmp, "k.conf"), "w") as f:
            f.write("relay = 127.0.0.1:8101\n")
        pid, fd = pty.fork()
        if pid == 0:
            os.chdir(tmp)
            os.environ.update(HOME=tmp, TERM="xterm", AGENT_HOST="fake.local",
                              GORK_CONFIG=os.path.join(tmp, "k.conf"))
            os.execv(BINARY, [BINARY])

        out, step, early, deadline = b"", 0, False, time.time() + 20
        while time.time() < deadline and step < 4:
            r, _, _ = select.select([fd], [], [], 0.1)
            if r:
                try:
                    out += os.read(fd, 4096)
                except OSError:
                    break
            if step == 0 and b"Enter send" in out:
                os.write(fd, b"hi\r")
                step = 1
            elif step == 1 and b"EARLY words" in out:
                early = b"LATER" not in out
                go_on.set()
                step = 2
            elif step == 2 and b"LATER ones" in out:
                step = 3
            elif step == 3 and idle_after(out, b"LATER ones"):
                os.write(fd, b"/quit\r")
                step = 4
        go_on.set()
        if step < 4:
            os.kill(pid, 9)
        status = reap(pid, fd)
    srv.shutdown()

    text = out.decode(errors="replace")
    assert step == 4, (step, text[-2000:])
    assert early, "the first words came only with the last"
    assert os.waitstatus_to_exitcode(status) == 0
    assert turns[0]["stream"] is True

    print("ok: the TUI shows a reply as it arrives")


# ------------------------------------------------------------- compact

def check_auto_compact():
    """Past auto_compact percent of context_window, gork swaps the
    conversation for the model's summary, mid-task, and carries on."""
    turns = []

    class H(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"

        def do_POST(self):
            req = json.loads(self.rfile.read(int(self.headers["content-length"])))
            turns.append(req)
            if len(turns) == 1:
                reply(self, {"id": "m1", "stop_reason": "tool_use",
                             "usage": {"input_tokens": 700, "output_tokens": 150},
                             "content": [{"type": "tool_use", "id": "toolu_01",
                                          "name": "read_file",
                                          "input": {"path": "testing/req.json"}}]})
            elif len(turns) == 2:
                reply(self, {"id": "m2", "stop_reason": "end_turn",
                             "usage": {"input_tokens": 900, "output_tokens": 40},
                             "content": [{"type": "text", "text": "SUMMARY-X"}]})
            else:
                reply(self, {"id": "m3", "stop_reason": "end_turn",
                             "content": [{"type": "text", "text": "done"}]})

        def log_message(self, *a):
            pass

    srv = serve(H, 8095)
    try:
        proc = run_gork(8095, "go", cwd=os.path.dirname(os.path.dirname(FAKEMCP)),
                         config="relay = 127.0.0.1:8095\ncontext_window = 1000\n"
                                "auto_compact = 80\n")
    finally:
        srv.shutdown()

    assert proc.returncode == 0, f"exit {proc.returncode}: {proc.stderr}"
    assert len(turns) == 3, len(turns)
    last = turns[1]["messages"][-1]
    assert last["role"] == "user", last
    kinds = [b["type"] for b in last["content"]]
    assert kinds == ["tool_result", "text"], kinds
    assert "Summarize" in last["content"][1]["text"]
    assert "tools" in turns[1], "tool_use history needs tools declared"
    msgs = turns[2]["messages"]
    assert len(msgs) == 1 and msgs[0]["role"] == "user", msgs
    assert "SUMMARY-X" in msgs[0]["content"] and "Continue" in msgs[0]["content"]
    assert proc.stdout.strip() == "done", proc.stdout
    assert "compacted" in proc.stderr, proc.stderr

    proc = run_gork(8095, "hi", config="relay = 127.0.0.1:8095\nauto_compact = 100\n")
    assert proc.returncode == 2 and "auto_compact" in proc.stderr, proc.stderr

    print("ok: auto compact")


def check_tui_abort():
    """Esc abandons a slow request; the conversation keeps the turn, and the
    status bar shows the context size once a reply reports usage."""
    turns, release = [], threading.Event()

    class H(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"

        def do_POST(self):
            turns.append(json.loads(self.rfile.read(int(self.headers["content-length"]))))
            if len(turns) == 1:
                release.wait(10)        # hang until gork gives up on us
                return
            reply(self, {"id": "m", "stop_reason": "end_turn",
                         "usage": {"input_tokens": 41000, "output_tokens": 1000},
                         "content": [{"type": "text", "text": "fine"}]})

        def log_message(self, *a):
            pass

    srv = serve(H, 8096, threaded=True)
    with tempfile.TemporaryDirectory() as tmp:
        tmp = os.path.realpath(tmp)
        os.makedirs(os.path.join(tmp, ".gork"))
        with open(os.path.join(tmp, ".gork", "trusted"), "w") as f:
            f.write(f"trust {tmp}\n")
        with open(os.path.join(tmp, "k.conf"), "w") as f:
            f.write("relay = 127.0.0.1:8096\n")
        pid, fd = pty.fork()
        if pid == 0:
            os.chdir(tmp)
            os.environ.update(HOME=tmp, TERM="xterm", AGENT_HOST="fake.local",
                              GORK_CONFIG=os.path.join(tmp, "k.conf"))
            os.execv(BINARY, [BINARY])

        out, step, deadline = b"", 0, time.time() + 15
        while time.time() < deadline and step < 5:
            r, _, _ = select.select([fd], [], [], 0.1)
            if r:
                try:
                    out += os.read(fd, 4096)
                except OSError:
                    break
            if step == 0 and b"Enter send" in out:
                os.write(fd, b"hi\r")
                step = 1
            elif step == 1 and turns:
                os.write(fd, b"\x1b")
                step = 2
            elif step == 2 and b"aborted" in out:
                os.write(fd, b"again\r")
                step = 3
            elif step == 3 and b"fine" in out:
                step = 4
            elif step == 4 and idle_after(out, b"42k/200k (21%)"):
                os.write(fd, b"/quit\r")
                step = 5
        release.set()
        if step < 5:
            os.kill(pid, 9)
        status = reap(pid, fd)
    srv.shutdown()

    text = out.decode(errors="replace")
    assert step == 5, (step, text[-2000:])
    assert os.waitstatus_to_exitcode(status) == 0
    roles = [(m["role"], m["content"]) for m in turns[1]["messages"]]
    assert roles == [("user", "hi"), ("assistant", "(aborted by the user)"),
                     ("user", "again")], roles

    print("ok: Esc abort, context in status bar")


def check_tui_tty_prompt():
    """A command that prompts on /dev/tty (ssh, ftp) gets a typed line,
    Enter included, while the TUI is up.  /effort sets the effort sent."""
    turns = []

    class H(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"

        def do_POST(self):
            turns.append(json.loads(self.rfile.read(int(self.headers["content-length"]))))
            if len(turns) == 1:
                reply(self, {"id": "m", "stop_reason": "tool_use", "content": [
                    {"type": "tool_use", "id": "t1", "name": "run_command", "input": {
                        "command": "printf 'P''w:' >/dev/tty; read -r x </dev/tty; echo got:$x"}}]})
            else:
                reply(self, {"id": "m", "stop_reason": "end_turn", "content": [
                    {"type": "text", "text": "done" if len(turns) == 2 else "resumed"}]})

        def log_message(self, *a):
            pass

    srv = serve(H, 8097)
    with tempfile.TemporaryDirectory() as tmp:
        tmp = os.path.realpath(tmp)
        os.makedirs(os.path.join(tmp, ".gork"))
        with open(os.path.join(tmp, ".gork", "trusted"), "w") as f:
            f.write(f"trust {tmp}\n")
        with open(os.path.join(tmp, "k.conf"), "w") as f:
            f.write("relay = 127.0.0.1:8097\n")
        pid, fd = pty.fork()
        if pid == 0:
            os.chdir(tmp)
            os.environ.update(HOME=tmp, TERM="xterm", AGENT_HOST="fake.local",
                              GORK_ALLOW_RUN="1",
                              GORK_CONFIG=os.path.join(tmp, "k.conf"))
            os.execv(BINARY, [BINARY])

        out, step, deadline = b"", 0, time.time() + 15
        while time.time() < deadline and step < 5:
            r, _, _ = select.select([fd], [], [], 0.1)
            if r:
                try:
                    out += os.read(fd, 4096)
                except OSError:
                    break
            if step == 0 and b"Enter send" in out:
                os.write(fd, b"/effort max\r")
                step = 0.5
            elif step == 0.5 and b"gork: effort max" in out:
                os.write(fd, b"go\r")
                step = 1
            elif step == 1 and b"Allow" in out:
                os.write(fd, b"y")
                step = 2
            elif step == 2 and b"Pw:" in out:   # only the command prints it joined up
                time.sleep(0.3)     # a human pause; bytes landing right on
                os.write(fd, b"secret\r")  # the mode switch skip ICRNL
                step = 3
            elif step == 3 and idle_after(out, b"done"):
                os.write(fd, b"/save\r")
                step = 3.1
            elif step == 3.1 and b"gork: saved 4 messages" in out:
                os.write(fd, b"/new\r")
                step = 3.2
            elif step == 3.2 and b"conversation cleared" in out:
                os.write(fd, b"/load\r")
                step = 3.3
            elif step == 3.3 and b"gork: loaded 4 messages" in out:
                os.write(fd, b"more\r")
                step = 4
            elif step == 4 and idle_after(out, b"resumed"):
                os.write(fd, b"/quit\r")
                step = 5
        if step < 5:
            os.kill(pid, 9)
        status = reap(pid, fd)
    srv.shutdown()

    assert step == 5, (step, out.decode(errors="replace")[-2000:])
    result = turns[1]["messages"][-1]["content"][0]["content"]
    assert "got:secret" in json.dumps(result), result
    assert turns[0]["output_config"] == {"effort": "max"}, "/effort max"
    assert len(turns[2]["messages"]) == 5, turns[2]["messages"]

    print("ok: run_command can prompt on /dev/tty, /effort, /save and /load")


def check_tui_mcp_esc_and_editing():
    """Esc stops a running MCP call, and the server is told.  Then the input
    line: the cursor keys to mend a word, Up to send a line again, and an
    input of two lines."""
    turns = []

    class H(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"

        def do_POST(self):
            turns.append(json.loads(self.rfile.read(int(self.headers["content-length"]))))
            if len(turns) == 1:
                reply(self, {"id": "m1", "stop_reason": "tool_use", "content": [
                    {"type": "tool_use", "id": "t0", "name": "mcp__fake__echo",
                     "input": {"text": "slow"}}]})
            else:
                reply(self, {"id": "m", "stop_reason": "end_turn", "content": [
                    {"type": "text", "text": "reply%d" % len(turns)}]})

        def log_message(self, *a):
            pass

    srv = serve(H, 8102)
    with tempfile.TemporaryDirectory() as tmp:
        tmp = os.path.realpath(tmp)
        log = os.path.join(tmp, "mcp.log")
        os.makedirs(os.path.join(tmp, ".gork"))
        with open(os.path.join(tmp, ".gork", "trusted"), "w") as f:
            f.write(f"trust {tmp}\n")
        with open(os.path.join(tmp, ".gork", "mcp.json"), "w") as f:
            json.dump({"mcpServers": {"fake": {"command": sys.executable,
                                               "args": [FAKEMCP]}}}, f)
        with open(os.path.join(tmp, "k.conf"), "w") as f:
            f.write("relay = 127.0.0.1:8102\nstream = no\n")
        pid, fd = pty.fork()
        if pid == 0:
            os.chdir(tmp)
            os.environ.update(HOME=tmp, TERM="xterm", AGENT_HOST="fake.local",
                              FAKEMCP_LOG=log,
                              GORK_CONFIG=os.path.join(tmp, "k.conf"))
            os.execv(BINARY, [BINARY])

        def logged():
            return open(log).read() if os.path.exists(log) else ""

        out, step, start, deadline = b"", 0, time.time(), time.time() + 25
        while time.time() < deadline and step < 7:
            r, _, _ = select.select([fd], [], [], 0.1)
            if r:
                try:
                    out += os.read(fd, 4096)
                except OSError:
                    break
            if step == 0 and b"Enter send" in out:
                os.write(fd, b"go\r")
                step = 1
            elif step == 1 and b"Allow" in out:
                os.write(fd, b"y")
                step = 2
            elif step == 2 and "started" in logged():
                time.sleep(0.3)
                os.write(fd, b"\x1b")
                step = 3
            elif step == 3 and b"gork: aborted" in out:
                stopped_after = time.time() - start
                # "agn", back one, "ai" before the n; End, "!"; Home, Delete
                # and "A": Again!  Then ^A, right, and Backspace takes the A.
                for keys in (b"agn", b"\x1bOD", b"ai", b"\x1bOF", b"!",
                             b"\x1bOH", b"\x1b[3~", b"A", b"\x01",
                             b"\x1bOC", b"\x7f", b"\r"):
                    os.write(fd, keys)
                    time.sleep(0.05)
                step = 4
            elif step == 4 and idle_after(out, b"reply2"):
                os.write(fd, b"x")
                time.sleep(0.05)
                os.write(fd, b"\x1bOA")             # Up: the line before
                time.sleep(0.05)
                os.write(fd, b"\x1bOA")             # and the first one
                time.sleep(0.05)
                os.write(fd, b"\x1bOB")             # Down again
                time.sleep(0.05)
                os.write(fd, b"\r")
                step = 5
            elif step == 5 and idle_after(out, b"reply3"):
                os.write(fd, b"\x1bOA")
                time.sleep(0.05)
                os.write(fd, b"\x1bOB")             # Down: the empty draft
                time.sleep(0.05)
                # A backslash and Enter: a second line.  Up from it goes
                # to the first, not into the history.
                for keys in (b"la\\\r", b"st", b"\x1bOA", b"X", b"\r"):
                    os.write(fd, keys)
                    time.sleep(0.05)
                step = 6
            elif step == 6 and idle_after(out, b"reply4"):
                os.write(fd, b"/quit\r")
                step = 7
        if step < 7:
            os.kill(pid, 9)
        status = reap(pid, fd)
        notes = logged()
    srv.shutdown()

    assert step == 7, (step, out.decode(errors="replace")[-2000:])
    assert os.waitstatus_to_exitcode(status) == 0
    assert stopped_after < 15, "Esc waited for the call"
    assert "cancelled True" in notes, notes
    assert "aborted by the user" in json.dumps(turns[1]["messages"]), turns[1]
    said = [m["content"] for t in turns[1:] for m in t["messages"][-1:]]
    assert said == ["gain!", "gain!", "laX\nst"], said

    print("ok: Esc stops an MCP call; line editing and history")


def check_tui_mcp_http_esc():
    """Esc stops a call to a streamable HTTP server too: the request is
    dropped, the server is told by a POST of its own, and the session is
    deleted when gork ends."""
    turns = []

    class H(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"

        def do_POST(self):
            turns.append(json.loads(self.rfile.read(int(self.headers["content-length"]))))
            if len(turns) == 1:
                reply(self, {"id": "m1", "stop_reason": "tool_use", "content": [
                    {"type": "tool_use", "id": "t0", "name": "mcp__web__echo",
                     "input": {"text": "slow"}}]})
            else:
                reply(self, {"id": "m", "stop_reason": "end_turn", "content": [
                    {"type": "text", "text": "reply%d" % len(turns)}]})

        def log_message(self, *a):
            pass

    srv = serve(H, 8104)
    with tempfile.TemporaryDirectory() as tmp:
        tmp = os.path.realpath(tmp)
        log = os.path.join(tmp, "mcp.log")
        fake = fakemcp_http(8105, log)
        os.makedirs(os.path.join(tmp, ".gork"))
        with open(os.path.join(tmp, ".gork", "trusted"), "w") as f:
            f.write(f"trust {tmp}\n")
        with open(os.path.join(tmp, ".gork", "mcp.json"), "w") as f:
            json.dump({"mcpServers": {"web": {
                "url": "http://127.0.0.1:8105/mcp",
                "headers": {"X-Token": "t1"}}}}, f)
        with open(os.path.join(tmp, "k.conf"), "w") as f:
            f.write("relay = 127.0.0.1:8104\nstream = no\n")
        pid, fd = pty.fork()
        if pid == 0:
            os.chdir(tmp)
            os.environ.update(HOME=tmp, TERM="xterm", AGENT_HOST="fake.local",
                              GORK_CONFIG=os.path.join(tmp, "k.conf"))
            os.execv(BINARY, [BINARY])

        def logged():
            return open(log).read() if os.path.exists(log) else ""

        out, step, start, deadline = b"", 0, time.time(), time.time() + 25
        while time.time() < deadline and step < 4:
            r, _, _ = select.select([fd], [], [], 0.1)
            if r:
                try:
                    out += os.read(fd, 4096)
                except OSError:
                    break
            if step == 0 and b"Enter send" in out:
                os.write(fd, b"go\r")
                step = 1
            elif step == 1 and b"Allow" in out:
                os.write(fd, b"y")
                step = 2
            elif step == 2 and "started" in logged():
                time.sleep(0.3)
                os.write(fd, b"\x1b")
                step = 3
            elif step == 3 and b"gork: aborted" in out:
                stopped_after = time.time() - start
                os.write(fd, b"/quit\r")
                step = 4
        if step < 4:
            os.kill(pid, 9)
        status = reap(pid, fd)
        fake.kill()
        fake.wait()
        notes = logged()
    srv.shutdown()

    assert step == 4, (step, out.decode(errors="replace")[-2000:])
    assert os.waitstatus_to_exitcode(status) == 0
    assert stopped_after < 15, "Esc waited for the call"
    assert "cancelled True" in notes, notes
    assert "deleted s-1" in notes, notes

    print("ok: Esc stops a call to an HTTP MCP server")


def check_run_timeout_and_esc():
    """run_command dies at `timeout`, background children included, and
    Esc in the TUI interrupts it and aborts the task."""
    turns = []
    command = ["sleep 30 & echo $! >bg.pid; sleep 30"]

    class H(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"

        def do_POST(self):
            turns.append(json.loads(self.rfile.read(int(self.headers["content-length"]))))
            if len(turns) == 1:
                reply(self, {"id": "m", "stop_reason": "tool_use", "content": [
                    {"type": "tool_use", "id": "t1", "name": "run_command",
                     "input": {"command": command[0]}}]})
            else:
                reply(self, {"id": "m", "stop_reason": "end_turn",
                             "content": [{"type": "text", "text": "done"}]})

        def log_message(self, *a):
            pass

    srv = serve(H, 8098)
    try:
        with tempfile.TemporaryDirectory() as tmp:
            start = time.time()
            proc = run_gork(8098, "go", cwd=tmp, GORK_ALLOW_RUN="1",
                            AGENT_TIMEOUT="1")
            assert time.time() - start < 10, "timeout not enforced"
            assert proc.returncode == 0, proc.stderr
            res = turns[1]["messages"][-1]["content"][0]
            assert res.get("is_error") and "timed out after 1 s" in json.dumps(res), res
            with open(os.path.join(tmp, "bg.pid")) as f:
                bg = int(f.read())
            # Give the kill a moment to land: on GNU/Hurd under emulation
            # the child is still there 0.2 s later.
            deadline = time.time() + 5
            while True:
                try:
                    os.kill(bg, 0)
                except ProcessLookupError:
                    break
                assert time.time() < deadline, \
                    "background child survived the timeout"
                time.sleep(0.1)

        # Esc while the command runs, in the TUI.
        del turns[:]
        # The marker (shown split in the TUI, printed joined) says the command
        # is really running; "Esc aborts" alone appears before it starts.
        command[0] = "printf 'UP''NOW' >/dev/tty; sleep 30"
        with tempfile.TemporaryDirectory() as tmp:
            tmp = os.path.realpath(tmp)
            os.makedirs(os.path.join(tmp, ".gork"))
            with open(os.path.join(tmp, ".gork", "trusted"), "w") as f:
                f.write(f"trust {tmp}\n")
            with open(os.path.join(tmp, "k.conf"), "w") as f:
                f.write("relay = 127.0.0.1:8098\n")
            pid, fd = pty.fork()
            if pid == 0:
                os.chdir(tmp)
                os.environ.update(HOME=tmp, TERM="xterm", AGENT_HOST="fake.local",
                                  GORK_ALLOW_RUN="1",
                                  GORK_CONFIG=os.path.join(tmp, "k.conf"))
                os.execv(BINARY, [BINARY])

            out, step, deadline = b"", 0, time.time() + 15
            while time.time() < deadline and step < 5:
                r, _, _ = select.select([fd], [], [], 0.1)
                if r:
                    try:
                        out += os.read(fd, 4096)
                    except OSError:
                        break
                if step == 0 and b"Enter send" in out:
                    os.write(fd, b"go\r")
                    step = 1
                elif step == 1 and b"Allow" in out:
                    os.write(fd, b"y")
                    step = 2
                elif step == 2 and b"UPNOW" in out:
                    time.sleep(0.3)
                    os.write(fd, b"\x1b")
                    step = 3
                elif step == 3 and b"gork: aborted" in out:
                    os.write(fd, b"again\r")
                    step = 4
                elif step == 4 and idle_after(out, b"done"):
                    os.write(fd, b"/quit\r")
                    step = 5
            if step < 5:
                os.kill(pid, 9)
            status = reap(pid, fd)
    finally:
        srv.shutdown()

    assert step == 5, (step, out.decode(errors="replace")[-2000:])
    assert os.waitstatus_to_exitcode(status) == 0
    assert "interrupted by the user" in json.dumps(turns[-1]["messages"]), turns[-1]

    print("ok: run_command timeout, Esc interrupts it")


def has_tui():
    """Does this build have a TUI?  Without one, gork refuses to start
    with no task (--without-tui)."""
    with tempfile.TemporaryDirectory() as tmp:
        conf = os.path.join(tmp, "c.conf")
        with open(conf, "w") as f:
            f.write("relay = 127.0.0.1:1\n")
        os.chmod(conf, 0o600)
        env = dict(os.environ, ANTHROPIC_API_KEY="sk-test", HOME=tmp,
                   GORK_CONFIG=conf)
        try:
            p = subprocess.run([BINARY], cwd=tmp, env=env, capture_output=True,
                               stdin=subprocess.DEVNULL, timeout=10)
        except subprocess.TimeoutExpired:
            return True
    return b"this build has no TUI" not in p.stderr


def has_mcp():
    """Does this build offer the tools of a server in ~/.gork/mcp.json?"""
    tools = []

    class H(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"

        def do_POST(self):
            req = json.loads(self.rfile.read(int(self.headers["content-length"])))
            tools.extend(t["name"] for t in req["tools"])
            reply(self, {"id": "m", "stop_reason": "end_turn",
                         "content": [{"type": "text", "text": "done"}]})

        def log_message(self, *a):
            pass

    with tempfile.TemporaryDirectory() as home:
        os.makedirs(os.path.join(home, ".gork"))
        with open(os.path.join(home, ".gork", "mcp.json"), "w") as f:
            json.dump({"mcpServers": {"fake": {"command": sys.executable,
                                               "args": [FAKEMCP]}}}, f)
        srv = serve(H, 8106)
        try:
            proc = run_gork(8106, "go", HOME=home)
        finally:
            srv.shutdown()
    assert proc.returncode == 0, f"MCP probe: exit {proc.returncode}: {proc.stderr}"
    return any(n.startswith("mcp__") for n in tools)


out = subprocess.run([BINARY, "--version"], capture_output=True, text=True)
assert out.returncode == 0 and re.match(r"gork v?\d+\.\d+", out.stdout), out
print("ok: --version")
FEATURES = {"TUI": has_tui(), "MCP": has_mcp()}
print("build: " + ", ".join(("" if v else "no ") + k for k, v in FEATURES.items())
      + (", through gork-relay" if RELAY else ""))
skipped = []


def run(check, *needs):
    """Run check, unless the build lacks something it needs."""
    missing = [n for n in needs if not FEATURES[n]]
    if missing:
        skipped.append(check.__name__)
        print("skip: %s (no %s)" % (check.__name__, " or ".join(missing)))
    else:
        check()


run(check_tool_loop)
run(check_openai)
run(check_rules_and_tools)
run(check_local_no_key)
run(check_key_still_required_for_anthropic)
run(check_allow_and_edit)
run(check_list_dir)
run(check_trust, "TUI")
run(check_skills)
run(check_mcp, "MCP")
run(check_mcp_http, "MCP")
run(check_config)
run(check_chunked)
run(check_stream)
run(check_stream_openai)
run(check_tui_stream, "TUI")
run(check_auto_compact)
run(check_tui_abort, "TUI")
run(check_tui_tty_prompt, "TUI")
run(check_tui_mcp_esc_and_editing, "TUI", "MCP")
run(check_tui_mcp_http_esc, "TUI", "MCP")
run(check_run_timeout_and_esc, "TUI")
print("all checks passed" + (" (%d skipped: not in this build)" % len(skipped) if skipped else ""))
