"""Checks for the ACP front end: gork as the agent an editor drives.

Usage:  python3 testing/fakeacp.py ./gork
Exits 0 only if every check passes.  Fails loudly on anything else.

gork --acp speaks JSON-RPC 2.0 on stdin/stdout to an ACP client, and nothing
else may appear on stdout.  This script is that client, with a fake Messages
API behind it, and it drives the whole path the protocol needs:

    initialize -> session/new -> a plain prompt -> a tool call approved
    -> a tool call refused -> a cancel in the middle of a reply -> exit

The cancel is the one that cannot be faked: gork has to read session/cancel
*while* the reply is still arriving, so the fake API holds that reply open
and the check times how long the stopReason takes to come back.
"""
import json, os, queue, select, shutil, subprocess, sys, tempfile
import http.server, threading, time

BINARY = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "./gork")


def ok(what):
    print("ok: " + what)


def fail(what):
    raise AssertionError(what)


# --------------------------------------------------------------- fake API

class Turn:
    """One scripted answer from the fake Messages API."""

    def __init__(self, body, delay=0.0):
        self.body, self.delay = body, delay


class FakeAPI:
    """A Messages API that answers from a list, and records the requests."""

    def __init__(self, port):
        self.port, self.turns, self.seen = port, [], []
        outer = self

        class H(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.0"

            def do_POST(self):
                n = int(self.headers.get("content-length", "0"))
                try:
                    req = json.loads(self.rfile.read(n))
                except ValueError:
                    req = None
                outer.seen.append(req)
                turn = outer.turns[len(outer.seen) - 1] \
                    if len(outer.seen) <= len(outer.turns) else None
                if turn is None:
                    fail("the fake API has no answer for request %d"
                         % len(outer.seen))
                if turn.delay:
                    time.sleep(turn.delay)
                body = json.dumps(turn.body).encode()
                try:
                    self.send_response(200)
                    self.send_header("content-type", "application/json")
                    self.send_header("content-length", str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)
                except (BrokenPipeError, ConnectionResetError):
                    pass            # gork stopped waiting: the cancel check

            def log_message(self, *a):
                pass

        self.srv = http.server.ThreadingHTTPServer(("127.0.0.1", port), H)
        self.srv.daemon_threads = True
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()

    def add(self, *turns):
        self.turns.extend(turns)


def answer(text):
    return Turn({"id": "msg_1", "stop_reason": "end_turn", "content": [
        {"type": "text", "text": text}]})


def tool_use(tool_id, name, inp):
    return Turn({"id": "msg_t", "stop_reason": "tool_use", "content": [
        {"type": "tool_use", "id": tool_id, "name": name, "input": inp}]})


# ------------------------------------------------------------------ client

class Client:
    """gork --acp, and the client side of the wire."""

    def __init__(self, cwd, config):
        env = dict(os.environ, HOME=cwd, GORK_CONFIG=config, TERM="dumb")
        for k in ("GORK_ALLOW_RUN", "AGENT_MODEL", "AGENT_PATH"):
            env.pop(k, None)
        self.proc = subprocess.Popen([BINARY, "--acp"], cwd=cwd, env=env,
                                     stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE)
        self.lines, self.bad = [], []
        self.lock = threading.Lock()
        self.pos = 0            # where the checks have read up to, in order
        self.id = 0
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for raw in self.proc.stdout:
            line = raw.decode("utf-8", "replace").strip()
            if not line:
                continue
            try:
                msg = json.loads(line)
            except ValueError:
                with self.lock:
                    self.bad.append(line)       # stdout is not the protocol
                continue
            with self.lock:
                self.lines.append(msg)

    def send(self, method, params=None):
        self.id += 1
        msg = {"jsonrpc": "2.0", "id": self.id, "method": method}
        if params is not None:
            msg["params"] = params
        self._write(msg)
        return self.id

    def notify(self, method, params=None):
        msg = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            msg["params"] = params
        self._write(msg)

    def reply(self, req_id, result):
        self._write({"jsonrpc": "2.0", "id": req_id, "result": result})

    def _write(self, msg):
        self.proc.stdin.write((json.dumps(msg) + "\n").encode())
        self.proc.stdin.flush()

    def wait(self, pred, what, timeout=15):
        """The next message pred() takes, after everything read so far.

        The wire is read in order and each check consumes what it needs, so
        a check that reuses a kind of update (tool_call_update twice) sees
        the second one, not the first again.
        """
        end = time.time() + timeout
        while time.time() < end:
            with self.lock:
                while self.pos < len(self.lines):
                    msg = self.lines[self.pos]
                    self.pos += 1
                    if pred(msg):
                        return msg
            time.sleep(0.02)
        with self.lock:
            seen = [json.dumps(m)[:200] for m in self.lines]
        fail("no %s within %gs; the client printed: %r" % (what, timeout, seen))

    def result(self, req_id, timeout=15):
        return self.wait(lambda m: m.get("id") == req_id and "result" in m,
                         "reply to request %d" % req_id, timeout)["result"]

    def update(self, kind, timeout=15):
        """The next session/update of that sessionUpdate kind."""
        return self.wait(
            lambda m: m.get("method") == "session/update" and
            m["params"]["update"].get("sessionUpdate") == kind,
            "session/update " + kind, timeout)

    def ask(self, timeout=15):
        return self.wait(
            lambda m: m.get("method") == "session/request_permission",
            "session/request_permission", timeout)

    def close(self):
        try:
            self.proc.stdin.close()
        except (BrokenPipeError, ValueError):
            pass
        try:
            return self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            fail("gork --acp did not exit")


def approve(req, option_id):
    return {"outcome": {"outcome": "selected", "optionId": option_id}}


# ------------------------------------------------------------------- checks

def check_a_session(tmp, api):
    """initialize, session/new, a plain prompt: the whole round trip."""
    conf = os.path.join(tmp, "k.conf")
    with open(conf, "w") as f:
        f.write("relay = 127.0.0.1:%d\napi_key = sk-test\n"
                "model = claude-opus-5\n" % api.port)
    os.chmod(conf, 0o600)
    c = Client(tmp, conf)

    r = c.result(c.send("initialize", {"protocolVersion": 1,
                                       "clientCapabilities": {}}))
    assert r["protocolVersion"] == 1, r
    assert r["agentInfo"]["name"] == "gork", r
    assert "version" in r["agentInfo"], r
    assert r["authMethods"] == [], "gork's key is not the editor's: %r" % r
    assert r["agentCapabilities"]["loadSession"] is False, r
    assert r["agentCapabilities"]["promptCapabilities"]["image"] is False, r
    ok("initialize answers protocolVersion 1, agentInfo, no authMethods")

    r = c.result(c.send("session/new", {"cwd": tmp, "mcpServers": []}))
    sid = r["sessionId"]
    assert sid.startswith("gork-"), r
    ok("session/new takes cwd and returns %s" % sid)

    api.add(answer("hello from fake"))
    pid = c.send("session/prompt", {"sessionId": sid, "prompt":
                 [{"type": "text", "text": "hi"}]})
    # in order, as an editor reads it: the text, the usage, then the result
    chunk = c.update("agent_message_chunk")
    assert chunk["params"]["sessionId"] == sid, chunk
    assert chunk["params"]["update"]["content"]["text"] == "hello from fake", \
        chunk
    c.update("usage_update")
    r = c.result(pid)
    assert r["stopReason"] == "end_turn", r
    ok("session/prompt streams agent_message_chunk, usage_update, end_turn")

    return c, sid


def check_tool_approved(c, sid, api, tmp):
    """A tool call the editor allows: its card, its result, the real write."""
    path = os.path.join(tmp, "written.txt")
    api.add(tool_use("toolu_a", "write_file",
                     {"path": path, "content": "written by the tool\n"}),
            answer("wrote it"))

    pid = c.send("session/prompt", {"sessionId": sid,
                                    "prompt": [{"type": "text",
                                                "text": "write it"}]})
    # the card first, then the ask for that card
    card = c.update("tool_call")["params"]["update"]
    assert card["toolCallId"] == "toolu_a", card
    assert card["kind"] == "edit", "write_file is an edit card: %r" % card
    assert card["status"] == "pending", card

    req = c.ask()
    p = req["params"]
    assert p["sessionId"] == sid, p
    assert p["toolCall"]["toolCallId"] == "toolu_a", p
    kinds = [o["kind"] for o in p["options"]]
    assert kinds == ["allow_once", "allow_always", "reject_once"], kinds
    c.reply(req["id"], approve(req, "allow-once"))

    done = c.update("tool_call_update")["params"]["update"]
    assert done["toolCallId"] == "toolu_a", done
    assert done["status"] == "completed", done
    assert c.result(pid)["stopReason"] == "end_turn"

    with open(path) as f:
        assert f.read() == "written by the tool\n", "the tool did not write it"
    ok("an approved tool call: request_permission, tool_call card, write")


def check_tool_refused(c, sid, api, tmp):
    """Refused: the card fails and the model is told, not run."""
    path = os.path.join(tmp, "refused.txt")
    api.add(tool_use("toolu_r", "write_file", {"path": path, "content": "no\n"}),
            answer("declined it"))

    pid = c.send("session/prompt", {"sessionId": sid,
                                    "prompt": [{"type": "text",
                                                "text": "write it again"}]})
    card = c.update("tool_call")["params"]["update"]
    assert card["toolCallId"] == "toolu_r", card
    req = c.ask()
    c.reply(req["id"], approve(req, "reject-once"))

    done = c.update("tool_call_update")["params"]["update"]
    assert done["status"] == "failed", done
    assert c.result(pid)["stopReason"] == "end_turn"
    assert not os.path.exists(path), "a refused write happened anyway"

    # and the model saw a tool_result saying so, rather than nothing
    last = api.seen[-1]
    res = last["messages"][-1]["content"]
    assert res[0]["type"] == "tool_result", res
    assert "declined" in res[0]["content"], res[0]
    ok("a refused tool call fails its card, writes nothing, tells the model")


def check_cancel(c, sid, api):
    """session/cancel while the reply is still arriving, not after it."""
    api.add(Turn({"id": "msg_slow", "stop_reason": "end_turn", "content": [
        {"type": "text", "text": "too late"}]}, delay=10.0))

    started = time.time()
    pid = c.send("session/prompt", {"sessionId": sid,
                                    "prompt": [{"type": "text",
                                                "text": "think hard"}]})
    time.sleep(0.4)                 # gork is now waiting on the API
    c.notify("session/cancel", {"sessionId": sid})
    r = c.result(pid, timeout=5)    # the reply is 10 s away, the cancel is not
    took = time.time() - started
    assert r["stopReason"] == "cancelled", r
    assert took < 5, "the cancel only landed after the reply: %.1fs" % took
    ok("session/cancel stops a reply in flight (%.1fs)" % took)


def check_stdout_is_protocol(c):
    """Not one byte of anything else on stdout: an editor's whole input."""
    with c.lock:
        bad, n = list(c.bad), len(c.lines)
    assert not bad, "non-JSON on stdout: %r" % bad[:5]
    assert n > 8, "too few messages to call it a session: %d" % n
    for m in list(c.lines):
        assert m.get("jsonrpc") == "2.0", m
    ok("stdout carried %d JSON-RPC messages and nothing else" % n)


def check_exit(c):
    """exit is answered, and gork leaves on it rather than on EOF."""
    rid = c.send("exit")
    c.result(rid)
    # stdin stays open: only the exit message can end this
    rc = c.close()
    assert rc == 0, "exit status %r" % rc
    ok("exit is answered and gork exits 0 with stdin still open")


def main():
    out = subprocess.run([BINARY, "--version"], capture_output=True, text=True)
    if out.returncode != 0:
        fail("this is not a gork binary: %r" % out)

    tmp = tempfile.mkdtemp(prefix="fakeacp.")
    api = FakeAPI(8211)
    c = None
    try:
        c, sid = check_a_session(tmp, api)
        check_tool_approved(c, sid, api, tmp)
        check_tool_refused(c, sid, api, tmp)
        check_cancel(c, sid, api)
        check_stdout_is_protocol(c)
    finally:
        if c is not None and c.proc.poll() is None:
            c.proc.kill()
            c.proc.wait()
        api.srv.shutdown()
        api.srv.server_close()

    # exit last, on its own process, so the kill above cannot hide the status
    tmp2 = tempfile.mkdtemp(prefix="fakeacp2.")
    try:
        with open(os.path.join(tmp2, "k.conf"), "w") as f:
            f.write("relay = 127.0.0.1:%d\napi_key = sk-test\nmodel = m\n"
                    % api.port)
        os.chmod(os.path.join(tmp2, "k.conf"), 0o600)
        c2 = Client(tmp2, os.path.join(tmp2, "k.conf"))
        c2.result(c2.send("initialize", {"protocolVersion": 1,
                                         "clientCapabilities": {}}))
        check_exit(c2)
    finally:
        shutil.rmtree(tmp2, ignore_errors=True)
        shutil.rmtree(tmp, ignore_errors=True)

    print("all checks passed")


if __name__ == "__main__":
    main()
