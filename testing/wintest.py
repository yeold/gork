"""Checks for the Windows gork.exe, run against fake Messages API servers.

Usage:  python testing\\wintest.py gork.exe          on Windows
        python3 testing/wintest.py wine gork.exe     elsewhere, under Wine
Exits 0 only if every check passes.  Batch mode only: the TUI needs a
console, which neither a CI runner nor a pipe is.
"""
import http.server, json, os, subprocess, sys, tempfile, threading, time

CMD = sys.argv[1:-1] + [os.path.abspath(sys.argv[-1])]
WINE = os.name != "nt"


def win(path):
    """The path as gork.exe sees it."""
    return "Z:" + path.replace("/", "\\") if WINE else path


def serve(handler_cls, port):
    srv = http.server.HTTPServer(("127.0.0.1", port), handler_cls)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


def run_gork(task, cwd, config=None, **env_extra):
    env = dict(os.environ, ANTHROPIC_API_KEY="sk-test", AGENT_HOST="fake.local",
               WINEDEBUG="-all")
    for k in ("HOME", "GORK_ALLOW_RUN", "GORK_CONFIG", "AGENT_MODEL", "AGENT_PATH"):
        env.pop(k, None)
    env.update(env_extra)
    conf = None
    if config is not None:
        conf = os.path.join(cwd, "..", "gork.conf")
        with open(conf, "w") as f:
            f.write(config)
        env["GORK_CONFIG"] = win(os.path.abspath(conf))
    return subprocess.run(CMD + [task], cwd=cwd, capture_output=True, text=True,
                          timeout=120, env=env)


def reply(handler, obj):
    body = json.dumps(obj).encode()
    handler.send_response(200)
    handler.send_header("content-type", "application/json")
    handler.send_header("content-length", str(len(body)))
    handler.end_headers()
    handler.wfile.write(body)


def tool_server(port, calls, turns):
    """First reply asks for every call in `calls`, the second says done."""
    class H(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"

        def do_POST(self):
            req = json.loads(self.rfile.read(int(self.headers["content-length"])))
            turns.append(({k.lower(): v for k, v in self.headers.items()}, req))
            if len(turns) == 1:
                reply(self, {"id": "m1", "stop_reason": "tool_use", "content": [
                    {"type": "tool_use", "id": f"t{i}", "name": n, "input": a}
                    for i, (n, a) in enumerate(calls)]})
            else:
                reply(self, {"id": "m2", "stop_reason": "end_turn",
                             "content": [{"type": "text", "text": "done"}]})

        def log_message(self, *a):
            pass

    return serve(H, port)


def results(turns):
    return {r["tool_use_id"]: r for r in turns[1][1]["messages"][2]["content"]}


def check_files():
    """File tools: both slashes, either case, CR LF kept, nothing outside."""
    with tempfile.TemporaryDirectory() as tmp:
        tmp = os.path.realpath(tmp)
        work, other = os.path.join(tmp, "work"), os.path.join(tmp, "other")
        os.makedirs(os.path.join(work, "Sub"))
        os.mkdir(other)
        with open(os.path.join(work, "Sub", "in.txt"), "wb") as f:
            f.write(b"one\r\ntwo\r\n")
        with open(os.path.join(work, "GORK.md"), "w") as f:
            f.write("Be brief.\n")
        with open(os.path.join(other, "secret.txt"), "w") as f:
            f.write("secret\n")
        with open(os.path.join(tmp, "allowed.txt"), "w") as f:
            f.write("allowed\n")
        os.mkdir(os.path.join(tmp, "also"))
        with open(os.path.join(tmp, "also", "a.txt"), "w") as f:
            f.write("also\n")

        calls = [
            ("read_file", {"path": "Sub\\in.txt"}),
            ("read_file", {"path": "sub/IN.TXT"}),
            ("write_file", {"path": "out.txt", "content": "a\nb\n"}),
            ("edit_file", {"path": "Sub/in.txt", "old_text": "one\ntwo",
                           "new_text": "1\n2"}),
            ("write_file", {"path": "..\\escape.txt", "content": "x"}),
            ("read_file", {"path": win(os.path.join(other, "secret.txt"))}),
            ("read_file", {"path": win(os.path.join(tmp, "also", "a.txt")).upper()}),
            ("list_dir", {"path": "."}),
            ("write_file", {"path": "Sub\\new\\x.txt", "content": "x"}),
        ]
        turns = []
        srv = tool_server(8101, calls, turns)
        try:
            proc = run_gork("go", work, "relay = 127.0.0.1:8101\nallow = %s ; %s\n"
                            % (win(os.path.join(tmp, "also")), win(work)))
        finally:
            srv.shutdown()

        assert proc.returncode == 0, f"exit {proc.returncode}: {proc.stderr}"
        assert "done" in proc.stdout, proc.stdout
        hdrs, first = turns[0]
        assert hdrs["x-api-key"] == "sk-test" and hdrs["host"] == "fake.local", hdrs
        assert first["system"].startswith("Be brief."), first["system"]
        names = [t["name"] for t in first["tools"]]
        assert names == ["list_dir", "read_file", "write_file", "edit_file"], names

        res = results(turns)
        assert res["t0"]["content"] == "one\ntwo\n", res["t0"]
        assert res["t1"]["content"] == "one\ntwo\n", res["t1"]
        assert "is_error" not in res["t2"], res["t2"]
        with open(os.path.join(work, "out.txt"), "rb") as f:
            assert f.read() == b"a\r\nb\r\n", "write_file should write CR LF"
        assert "is_error" not in res["t3"], res["t3"]
        with open(os.path.join(work, "Sub", "in.txt"), "rb") as f:
            assert f.read() == b"1\r\n2\r\n", "edit_file should replace the file"
        assert res["t4"].get("is_error") and "outside" in res["t4"]["content"], res["t4"]
        assert not os.path.exists(os.path.join(tmp, "escape.txt"))
        assert res["t5"].get("is_error") and "secret" not in res["t5"]["content"], res["t5"]
        assert res["t6"]["content"] == "also\n", res["t6"]
        assert "Sub/\n" in res["t7"]["content"] and "out.txt" in res["t7"]["content"], res["t7"]
        assert res["t8"].get("is_error"), res["t8"]

    print("ok: file tools, path confinement, allow directories")


def check_run_command():
    """run_command goes through the command interpreter; off unless allowed."""
    with tempfile.TemporaryDirectory() as tmp:
        calls = [("run_command", {"command": "echo hello& echo oops 1>&2"}),
                 ("run_command", {"command": "exit 3"})]
        turns = []
        srv = tool_server(8102, calls, turns)
        try:
            proc = run_gork("go", tmp, "relay = 127.0.0.1:8102\n", GORK_ALLOW_RUN="1")
        finally:
            srv.shutdown()
        assert proc.returncode == 0, f"exit {proc.returncode}: {proc.stderr}"
        assert "run_command" in [t["name"] for t in turns[0][1]["tools"]]
        res = results(turns)
        out = res["t0"]["content"]
        assert "hello" in out and "oops" in out and out.endswith("[exit 0]"), res["t0"]
        assert "\r" not in out, res["t0"]
        assert "is_error" not in res["t0"], res["t0"]
        assert res["t1"]["content"].endswith("[exit 3]") and res["t1"].get("is_error"), res["t1"]

    print("ok: run_command")


def check_home():
    """With no HOME, gork.cnf is read from %USERPROFILE%."""
    if WINE:                    # Wine sets USERPROFILE itself
        print("skipped: config from %USERPROFILE% (not under Wine)")
        return
    seen = []

    class H(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"

        def do_POST(self):
            seen.append(json.loads(self.rfile.read(int(self.headers["content-length"]))))
            reply(self, {"id": "m", "stop_reason": "end_turn",
                         "content": [{"type": "text", "text": "hello"}]})

        def log_message(self, *a):
            pass

    with tempfile.TemporaryDirectory() as tmp:
        with open(os.path.join(tmp, "gork.cnf"), "w") as f:
            f.write("relay = 127.0.0.1:8103\nmodel = from-profile\n")
        srv = serve(H, 8103)
        try:
            proc = run_gork("hi", tmp, USERPROFILE=win(tmp))
        finally:
            srv.shutdown()
    assert proc.returncode == 0, f"exit {proc.returncode}: {proc.stderr}"
    assert "hello" in proc.stdout and seen[0]["model"] == "from-profile", (proc.stdout, seen)

    print("ok: config from %USERPROFILE%")


def check_timeout():
    """A server that never answers: the polled deadline ends the request."""
    class H(http.server.BaseHTTPRequestHandler):
        def do_POST(self):
            time.sleep(8)

        def log_message(self, *a):
            pass

    with tempfile.TemporaryDirectory() as tmp:
        srv = serve(H, 8104)
        try:
            start = time.time()
            proc = run_gork("hi", tmp, "relay = 127.0.0.1:8104\ntimeout = 2\n"
                            "retries = 0\n")
            took = time.time() - start
        finally:
            srv.shutdown()
    assert proc.returncode == 1 and "timeout on socket" in proc.stderr, proc.stderr
    assert took < 7, f"took {took:.1f}s"

    proc = run_gork("hi", os.getcwd(), "relay = 127.0.0.1:8105\nretries = 0\n")
    assert proc.returncode == 1 and "Winsock error" in proc.stderr, proc.stderr

    print("ok: timeout, refused connection")


out = subprocess.run(CMD + ["--version"], capture_output=True, text=True,
                     env=dict(os.environ, WINEDEBUG="-all"))
assert out.returncode == 0 and out.stdout.startswith("gork "), out
print("ok: --version")
check_files()
check_run_command()
check_home()
check_timeout()
print("all Windows checks passed")
