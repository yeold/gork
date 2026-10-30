"""Checks for the Windows gork.exe, run against fake Messages API servers.

Usage:  python testing\\wintest.py gork.exe          on Windows
        python3 testing/wintest.py wine gork.exe     elsewhere, under Wine
Exits 0 only if every check passes.  Batch mode, and under Wine the TUI
too: Wine gives a program on a pty a console.  On Windows the TUI check is
skipped, as a CI runner has no console and a pipe is not one.

With $GORK_RELAY naming a gork-relay binary, every fake API server sits
behind it: the fake listens on port + 1000 and `gork-relay -p` on the
port gork is given.

With $GORK_NONET set as well, gork.exe is a -nonet build (docs/SERIAL.md),
under Wine: each fake sits behind `gork-relay -S` on one end of a socat
pseudo-terminal pair, Wine's COM1 is the other end, and every
configuration's relay becomes serial:COM1:38400.  Needs socat.
"""
import http.server, json, os, re, select, subprocess, sys, tempfile, threading, time

CMD = sys.argv[1:-1] + [os.path.abspath(sys.argv[-1])]
WINE = os.name != "nt"
RELAY = os.environ.get("GORK_RELAY")
NONET = bool(os.environ.get("GORK_NONET"))
COM1 = "/tmp/gork-wintest-com1"         # Wine's COM1, made by socat


def win(path):
    """The path as gork.exe sees it."""
    return "Z:" + path.replace("/", "\\") if WINE else path


class Served:
    """A fake server, and the gork-relay in front of it if there is one."""

    def __init__(self, srv, relay=None, line=None):
        self.srv, self.relay, self.line = srv, relay, line

    def shutdown(self):
        for p in (self.relay, self.line):
            if p is not None:
                p.kill()
                p.wait()
        self.srv.shutdown()
        self.srv.server_close()


def serve(handler_cls, port):
    srv = http.server.HTTPServer(("127.0.0.1", port + 1000 if RELAY else port),
                                 handler_cls)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    if not RELAY:
        return Served(srv)
    log = tempfile.TemporaryFile()
    line = None
    if NONET:                   # a null modem: COM1 to the relay's end
        far = COM1 + "-relay"
        line = subprocess.Popen(["socat", "pty,raw,echo=0,link=" + COM1,
                                 "pty,raw,echo=0,link=" + far])
        for _ in range(50):
            if os.path.exists(COM1) and os.path.exists(far):
                break
            time.sleep(0.1)
        listen = ["-t", "0", "-S", far + ":38400"]
    else:
        listen = ["-t", "127.0.0.1:%d" % port]
    relay = subprocess.Popen([RELAY, "-p", "-v"] + listen +
                             ["127.0.0.1:%d" % (port + 1000)],
                             stdout=subprocess.DEVNULL, stderr=log)
    end = time.time() + 10
    while True:
        log.seek(0)
        started = log.read()
        if b"TCP on" in started or (NONET and b"serial" in started.lower()):
            return Served(srv, relay, line)
        if relay.poll() is not None or time.time() > end:
            log.seek(0)
            srv.shutdown()
            raise AssertionError("gork-relay did not start on %d: %r"
                                 % (port, log.read()[-500:]))
        time.sleep(0.05)


def to_line(config):
    """With $GORK_NONET, a configuration's relay is the serial line."""
    return re.sub(r"relay = 127\.0\.0\.1:\d+", "relay = serial:COM1:38400",
                  config) if NONET else config


def run_gork(task, cwd, config=None, **env_extra):
    env = dict(os.environ, ANTHROPIC_API_KEY="sk-test", AGENT_HOST="fake.local",
               WINEDEBUG="-all")
    for k in ("HOME", "GORK_ALLOW_RUN", "GORK_CONFIG", "AGENT_MODEL", "AGENT_PATH"):
        env.pop(k, None)
    env.update(env_extra)
    conf = None
    if config is not None:
        config = to_line(config)
    if config is not None:
        conf = os.path.join(cwd, "..", "gork.conf")
        with open(conf, "w") as f:
            f.write(config)
        env["GORK_CONFIG"] = win(os.path.abspath(conf))
    return subprocess.run(CMD + [task], cwd=cwd, capture_output=True, text=True,
                          timeout=120, env=env)


def reply(handler, obj, stream=False):
    """`obj` as the reply: whole, or as an event stream if that was asked."""
    body = events(obj) if stream else json.dumps(obj).encode()
    handler.send_response(200)
    if stream:
        handler.send_header("content-type", "text/event-stream")
    else:
        handler.send_header("content-type", "application/json")
        handler.send_header("content-length", str(len(body)))
    handler.end_headers()
    for i in range(0, len(body), 500):
        handler.wfile.write(body[i:i + 500])
        handler.wfile.flush()

def events(msg):
    """A whole reply as the event stream it is when `"stream": true` asked
    for it: text and tool input each in two pieces."""
    out = [{"type": "message_start", "message": {
        "id": msg.get("id", "m"), "role": "assistant", "content": [],
        "usage": {"input_tokens": msg.get("usage", {}).get("input_tokens", 0),
                  "output_tokens": 1}}}]
    for i, b in enumerate(msg["content"]):
        if b["type"] == "text":
            t, h = b["text"], len(b["text"]) // 2
            out.append({"type": "content_block_start", "index": i,
                        "content_block": {"type": "text", "text": ""}})
            out += [{"type": "content_block_delta", "index": i,
                     "delta": {"type": "text_delta", "text": p}}
                    for p in (t[:h], t[h:])]
        else:
            j = json.dumps(b["input"])
            out.append({"type": "content_block_start", "index": i,
                        "content_block": dict(b, input={})})
            out += [{"type": "content_block_delta", "index": i,
                     "delta": {"type": "input_json_delta", "partial_json": p}}
                    for p in (j[:len(j) // 2], j[len(j) // 2:])]
        out.append({"type": "content_block_stop", "index": i})
    out.append({"type": "message_delta",
                "delta": {"stop_reason": msg["stop_reason"]},
                "usage": {"output_tokens": msg.get("usage", {}).get("output_tokens", 0)}})
    out.append({"type": "message_stop"})
    return b"".join(b"event: %s\ndata: %s\n\n" % (e["type"].encode(),
                                                   json.dumps(e).encode())
                    for e in out)



def tool_server(port, calls, turns):
    """First reply asks for every call in `calls`, the second says done."""
    class H(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"

        def do_POST(self):
            req = json.loads(self.rfile.read(int(self.headers["content-length"])))
            turns.append(({k.lower(): v for k, v in self.headers.items()}, req))
            stream = bool(req.get("stream"))
            if len(turns) == 1:
                reply(self, {"id": "m1", "stop_reason": "tool_use", "content": [
                    {"type": "tool_use", "id": f"t{i}", "name": n, "input": a}
                    for i, (n, a) in enumerate(calls)]}, stream)
            else:
                reply(self, {"id": "m2", "stop_reason": "end_turn",
                             "content": [{"type": "text", "text": "done"}]},
                      stream)

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


def check_stream():
    """`stream = yes`: tool calls and the answer arrive as an event stream
    and come out as they would whole."""
    turns = []
    with tempfile.TemporaryDirectory() as tmp:
        work = os.path.join(tmp, "work")
        os.mkdir(work)
        srv = tool_server(8086, [("write_file", {"path": "s.txt", "content": "streamed\n"}),
                                 ("read_file", {"path": "s.txt"})], turns)
        try:
            proc = run_gork("go", work, config="relay = 127.0.0.1:8086\nstream = yes\n")
        finally:
            srv.shutdown()
        assert proc.returncode == 0, proc.stderr
        assert turns[0][1]["stream"] is True
        r = results(turns)
        assert "wrote" in r["t0"]["content"], r
        assert r["t1"]["content"] == "streamed\n", r
        assert proc.stdout.replace("\r\n", "\n") == "done\n", repr(proc.stdout)
    print("ok: a streamed reply")


def check_home():
    """With no HOME, gork.cnf is read from %USERPROFILE%."""
    with tempfile.TemporaryDirectory() as tmp:
        proc = run_gork("hi", tmp)
    assert proc.returncode == 2 and "gork.cnf (or" in proc.stderr \
        and "~" not in proc.stderr, "the message should name the file: " + proc.stderr
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
            f.write(to_line("relay = 127.0.0.1:8103\nmodel = from-profile\n"))
        srv = serve(H, 8103)
        try:
            proc = run_gork("hi", tmp, USERPROFILE=win(tmp))
        finally:
            srv.shutdown()
    assert proc.returncode == 0, f"exit {proc.returncode}: {proc.stderr}"
    assert "hello" in proc.stdout and seen[0]["model"] == "from-profile", (proc.stdout, seen)

    print("ok: config from %USERPROFILE%")


def check_text():
    """A task with a non-ASCII letter reaches the API as UTF-8; the reply's
    UTF-8 comes back untouched when stdout is not a console."""
    seen = []

    class H(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"

        def do_POST(self):
            seen.append(self.rfile.read(int(self.headers["content-length"])))
            reply(self, {"id": "m", "stop_reason": "end_turn",
                         "content": [{"type": "text", "text": "na\u00efve \u2014 yes"}]})

        def log_message(self, *a):
            pass

    with tempfile.TemporaryDirectory() as tmp:
        srv = serve(H, 8106)
        try:
            env = dict(os.environ, AGENT_HOST="fake.local", WINEDEBUG="-all",
                       LC_ALL="C.UTF-8")
            env.pop("HOME", None)
            conf = os.path.join(tmp, "c.conf")
            with open(conf, "w") as f:
                f.write(to_line("relay = 127.0.0.1:8106\n"))
            env["GORK_CONFIG"] = win(conf)
            proc = subprocess.run(CMD + ["caf\u00e9?"], cwd=tmp, capture_output=True,
                                  timeout=120, env=env)
        finally:
            srv.shutdown()
    assert proc.returncode == 0, f"exit {proc.returncode}: {proc.stderr}"
    task = json.loads(seen[0].decode("utf-8"))["messages"][0]["content"]
    assert task == "caf\u00e9?", ascii(task)
    assert "na\u00efve \u2014 yes".encode("utf-8") in proc.stdout, proc.stdout

    print("ok: non-ASCII task and reply")


def check_timeout():
    """A server that never answers: the polled deadline ends the request."""
    release = threading.Event()

    class H(http.server.BaseHTTPRequestHandler):
        def do_POST(self):
            release.wait(30)

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
            release.set()
            srv.shutdown()
    assert proc.returncode == 1 and ("timeout on socket" in proc.stderr or
                                     "timeout on the serial line" in proc.stderr), proc.stderr
    # well before the server lets go; a busy machine starts Wine slowly
    assert took < 20, f"took {took:.1f}s"

    if NONET:                           # a port that is not there
        proc = run_gork("hi", os.getcwd(), "relay = serial:COM7\nretries = 0\n")
        assert proc.returncode == 1 and "COM7" in proc.stderr, proc.stderr
    else:
        proc = run_gork("hi", os.getcwd(), "relay = 127.0.0.1:8105\nretries = 0\n")
        assert proc.returncode == 1 and "Winsock error" in proc.stderr, proc.stderr

    print("ok: timeout, refused connection")


def plain(b):
    """Screen output with the escapes taken out; Wine draws a character
    at a time, with a cursor move for each run of spaces."""
    b = re.sub(rb"\x1b\[(\d*)C", lambda m: b" " * int(m.group(1) or 1), b)
    b = re.sub(rb"\x1b(\[[0-9;?]*[A-Za-z]|\][^\x07]*\x07|[()][0-9A-Z]|[=>])", b"", b)
    return b.replace(b"\r", b"").replace(b"\n", b"")


def check_tui_save_load():
    """The TUI on a pty: trust, a turn, and the replies to /effort, /save,
    /new and /load, which only show if stderr reaches the chat pane."""
    if not WINE:
        print("skip: TUI (needs a console)")
        return
    import pty
    turns = []

    class H(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"

        def do_POST(self):
            turns.append(json.loads(self.rfile.read(int(self.headers["content-length"]))))
            reply(self, {"id": "m", "stop_reason": "end_turn", "content": [
                {"type": "text", "text": "done" if len(turns) == 1 else "resumed"}]})

        def log_message(self, *a):
            pass

    steps = [(b"Trust this directory", b"y\r"), (b"/quit", b"hello\r"),
             (b"done", b"/effort max\r"), (b"gork: effort max", b"/save\r"),
             (b"saved 2 messages", b"/new\r"), (b"conversation cleared", b"/load\r"),
             (b"loaded 2 messages", b"more\r"), (b"resumed", b"/quit\r")]
    with tempfile.TemporaryDirectory() as tmp:
        tmp = os.path.realpath(tmp)
        with open(os.path.join(tmp, "gork.cnf"), "w") as f:
            f.write(to_line("relay = 127.0.0.1:8107\r\n"))
        env = dict(os.environ, WINEDEBUG="-all", ANTHROPIC_API_KEY="sk-test",
                   AGENT_HOST="fake.local", TERM="xterm",
                   GORK_CONFIG=win(tmp + "/gork.cnf"), USERPROFILE=win(tmp))
        for k in ("HOME", "GORK_ALLOW_RUN", "AGENT_MODEL", "AGENT_PATH"):
            env.pop(k, None)
        srv = serve(H, 8107)
        try:
            pid, fd = pty.fork()
            if pid == 0:
                os.chdir(tmp)
                os.execvpe(CMD[0], CMD, env)
            out, i, mark, sent_at, again = b"", 0, 0, 0.0, 0
            end = time.time() + 120
            while time.time() < end and i < len(steps):
                if select.select([fd], [], [], 0.5)[0]:
                    try:
                        d = os.read(fd, 65536)
                    except OSError:
                        break
                    if not d:
                        break
                    out += d
                if steps[i][0] in plain(out[mark:]):
                    time.sleep(0.5)
                    os.write(fd, steps[i][1])
                    mark, sent_at, again = len(out), time.time(), 0
                    i += 1
                elif (i > 0 and again < 3 and time.time() - sent_at > 5 and
                      (lambda s: s == b"" or s.endswith(steps[i - 1][1].strip()))(
                          re.sub(rb"\s|\^J|\^M", b"", plain(out[mark:])))):
                    # Nothing came of the keys but the terminal's echo, if
                    # that: typed before Wine read the console, which
                    # drops them.  Type them again.
                    os.write(fd, steps[i - 1][1])
                    mark, sent_at, again = len(out), time.time(), again + 1
            if i < len(steps):
                os.kill(pid, 9)
            os.waitpid(pid, 0)
            os.close(fd)
        finally:
            srv.shutdown()
        saved = os.path.exists(os.path.join(tmp, "_gork", "session.jsn"))
    assert i == len(steps), "stuck waiting for %r; the screen: %r" % (
        steps[i][0], plain(out[mark:])[-1500:])
    assert saved, "no _gork/session.jsn"
    assert turns[1].get("output_config") == {"effort": "max"}, "/effort max"
    assert len(turns) == 2 and len(turns[1]["messages"]) == 3, turns[-1]["messages"]

    print("ok: TUI, /effort, /save and /load")


if NONET:
    # COM1 is the socat end, by Wine's own map of ports (the prefix first)
    subprocess.run(["wineboot", "-i"], capture_output=True,
                   env=dict(os.environ, WINEDEBUG="-all"))
    subprocess.run(["wine", "reg", "add", "HKLM\\Software\\Wine\\Ports",
                    "/v", "COM1", "/t", "REG_SZ", "/d", COM1, "/f"],
                   check=True, capture_output=True,
                   env=dict(os.environ, WINEDEBUG="-all"))
    subprocess.run(["wineserver", "-w"])
out = subprocess.run(CMD + ["--version"], capture_output=True, text=True,
                     env=dict(os.environ, WINEDEBUG="-all"))
assert out.returncode == 0 and out.stdout.startswith("gork "), out
print("ok: --version")
check_files()
check_run_command()
check_stream()
check_home()
check_text()
check_timeout()
check_tui_save_load()
print("all Windows checks passed")
