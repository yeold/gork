"""Checks for the gork agent loop, run against fake Messages API servers.

Usage:  python3 testing/looptest.py ./gork
Exits 0 only if every check passes.  Fails loudly on anything else.
"""
import http.server, json, os, pty, select, subprocess, sys, tempfile, threading, time

BINARY = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "./gork")


def serve(handler_cls, port):
    """Run handler_cls on port until the returned shutdown() is called."""
    srv = http.server.HTTPServer(("127.0.0.1", port), handler_cls)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


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
        return subprocess.run([BINARY, task], cwd=cwd, capture_output=True,
                              text=True, timeout=30, env=env)


def reply(handler, obj, status=200):
    body = json.dumps(obj).encode()
    handler.send_response(status)
    handler.send_header("content-type", "application/json")
    handler.send_header("content-length", str(len(body)))
    handler.end_headers()
    handler.wfile.write(body)


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
    _, status = os.waitpid(pid, 0)
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


def check_mcp():
    """Servers in ~/.gork/mcp.json are started, their tools offered as
    mcp__<server>__<tool>, and calls round-trip.  A broken or disabled
    server is skipped without taking the others down."""
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
    assert "remote: no command" in proc.stderr, proc.stderr

    names = [t["name"] for t in turns[0]["tools"]]
    assert names[-2:] == ["mcp__fake__echo", "mcp__fake__fail"], names
    assert not [n for n in names if n.startswith("mcp__off")], names
    echo = turns[0]["tools"][-2]
    assert echo["input_schema"]["required"] == ["text"], echo

    res = {r["tool_use_id"]: r for r in turns[1]["messages"][2]["content"]}
    assert res["t0"]["content"] == "echo: hi\n[image content omitted]\n", res["t0"]
    assert "is_error" not in res["t0"], res["t0"]
    assert res["t1"].get("is_error") and "it broke" in res["t1"]["content"], res["t1"]

    print("ok: mcp")


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
    seen = []

    class H(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"

        def do_POST(self):
            req = json.loads(self.rfile.read(int(self.headers["content-length"])))
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
    proc = run_gork(8092, "hi", config="relay = 127.0.0.1:8092\ndefault_effort = igh\n")
    assert proc.returncode == 2 and "want default_effort" in proc.stderr, proc.stderr

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

def check_chunked():
    """A chunked reply must name itself, not surface as a JSON parse error."""
    class H(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"

        def do_POST(self):
            self.rfile.read(int(self.headers["content-length"]))
            self.wfile.write(b"HTTP/1.1 200 OK\r\n"
                             b"Content-Type: application/json\r\n"
                             b"Transfer-Encoding: chunked\r\n\r\n"
                             b"5\r\n{\"id\"\r\n0\r\n\r\n")

        def log_message(self, *a):
            pass

    srv = serve(H, 8094)
    try:
        proc = run_gork(8094, "hi")
    finally:
        srv.shutdown()

    assert proc.returncode != 0, "chunked reply should fail"
    assert "chunked" in proc.stderr, f"unhelpful error: {proc.stderr!r}"
    assert "JSON" not in proc.stderr, f"leaked past the check: {proc.stderr!r}"

    print("ok: chunked reply diagnosed")

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

    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 8096), H)
    srv.daemon_threads = True           # the hung first request must not block
    threading.Thread(target=srv.serve_forever, daemon=True).start()
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
            elif step == 4 and b"42k/200k (21%)" in out:
                os.write(fd, b"/quit\r")
                step = 5
        release.set()
        if step < 5:
            os.kill(pid, 9)
        _, status = os.waitpid(pid, 0)
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
                        "command": "printf Pw: >/dev/tty; read -r x </dev/tty; echo got:$x"}}]})
            else:
                reply(self, {"id": "m", "stop_reason": "end_turn",
                             "content": [{"type": "text", "text": "done"}]})

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
            elif step == 2 and b"Pw:" in out.split(b"running command")[-1]:
                time.sleep(0.3)     # a human pause; bytes landing right on
                os.write(fd, b"secret\r")  # the mode switch skip ICRNL
                step = 3
            elif step == 3 and b"done" in out:
                os.write(fd, b"/quit\r")
                step = 5
        if step < 5:
            os.kill(pid, 9)
        _, status = os.waitpid(pid, 0)
    srv.shutdown()

    assert step == 5, (step, out.decode(errors="replace")[-2000:])
    result = turns[1]["messages"][-1]["content"][0]["content"]
    assert "got:secret" in json.dumps(result), result
    assert turns[0]["output_config"] == {"effort": "max"}, "/effort max"

    print("ok: run_command can prompt on /dev/tty, /effort")


check_tool_loop()
check_rules_and_tools()
check_local_no_key()
check_key_still_required_for_anthropic()
check_allow_and_edit()
check_list_dir()
check_trust()
check_skills()
check_mcp()
check_config()
check_chunked()
check_auto_compact()
check_tui_abort()
check_tui_tty_prompt()
print("all checks passed")
