#!/usr/bin/env python3
"""Check the Cygwin package of gork on a Windows without Cygwin.

Usage:  python testing\\cygpacktest.py DIR      (the unpacked gork-*-win32)

Runs with the Windows Python; gork itself gets a PATH with nothing but
Windows in it, so that it must find its DLLs, its shell and its terminal
descriptions in the package.  A fake Messages API asks for a file to be
written and a command to be run, and the TUI is driven in a console of
its own (needs the pywinpty module; skipped without it, unless
GORK_NEED_TUI is set).  Exits 0 only if every check passes.
"""
import http.server, json, os, re, subprocess, sys, tempfile, threading, time

PKG = os.path.abspath(sys.argv[1])
PORT = 8094
requests = []
script = []


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def do_POST(self):
        requests.append(json.loads(self.rfile.read(int(self.headers["content-length"]))))
        body = json.dumps(script.pop(0)).encode()
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a):
        pass


def tool(name, **inp):
    return {"id": "m", "stop_reason": "tool_use",
            "content": [{"type": "tool_use", "id": "t1", "name": name, "input": inp}]}


def text(s):
    return {"id": "m", "stop_reason": "end_turn",
            "content": [{"type": "text", "text": s}]}


def gork(args, home, cwd, **extra):
    env = clean_env(home, **extra)
    return subprocess.run(["cmd", "/c", os.path.join(PKG, "gork.cmd")] + args,
                          cwd=cwd, env=env, capture_output=True, text=True,
                          timeout=120)


def clean_env(home, **extra):
    root = os.environ["SystemRoot"]
    env = {"SystemRoot": root, "PATH": root + "\\System32;" + root,
           "USERPROFILE": home, "TEMP": home, "TMP": home,
           "ComSpec": os.environ["ComSpec"], "GORK_ALLOW_RUN": "1"}
    env.update(extra)
    return env


def check_tui(home, work):
    """The TUI in a Windows console (a pseudo console): the trust prompt, a
    line typed, the reply, /quit.  (The cursor keys are left to looptest.py
    on Cygwin: a pseudo console does not pass their sequences on whole.)"""
    try:
        import winpty
    except ImportError:
        if os.environ.get("GORK_NEED_TUI"):
            raise
        print("skipped: the TUI (no pywinpty module)")
        return
    del requests[:]
    script[:] = [text("tui reply")]
    proc = winpty.PtyProcess.spawn(
        ["cmd", "/c", os.path.join(PKG, "gork.cmd")], cwd=work,
        env=clean_env(home), dimensions=(25, 100))
    seen = []

    def pump():
        try:
            while True:
                seen.append(proc.read(4096))
        except EOFError:
            pass

    threading.Thread(target=pump, daemon=True).start()

    def screen():
        """What was written, without escape sequences and blanks: the
        console moves the cursor where a program wrote spaces."""
        t = re.sub(r"\x1b\][^\x07]*\x07|\x1b\[[0-9;?]*[ -/]*[@-~]|\x1b.", "",
                   "".join(seen))
        return re.sub(r"\s+", "", t)

    def wait(what, secs=60):
        end = time.time() + secs
        while time.time() < end:
            if what in screen():
                return
            time.sleep(0.2)
        raise AssertionError("no %r in the console:\n%s" % (what, screen()[-3000:]))

    try:
        wait("Trustthisdirectory")
        proc.write("y\r")
        wait("Entersend")
        proc.write("helx\x7flo\r")              # with a Backspace
        wait("tuireply")
        assert requests[0]["messages"][0]["content"] == "hello", requests[0]
        proc.write("/quit\r")
        end = time.time() + 30
        while proc.isalive() and time.time() < end:
            time.sleep(0.2)
        assert not proc.isalive(), "still running after /quit"
        assert proc.exitstatus == 0, proc.exitstatus
    finally:
        if proc.isalive():
            proc.terminate(force=True)
    print("ok: the TUI in a Windows console")


def results():
    out = []
    for r in requests:
        last = r["messages"][-1]["content"]
        if isinstance(last, list):
            out += [b for b in last if b.get("type") == "tool_result"]
    return out


def main():
    srv = http.server.HTTPServer(("127.0.0.1", PORT), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    with tempfile.TemporaryDirectory() as home:
        work = os.path.join(home, "work")
        os.mkdir(work)
        r = gork(["--version"], home, work)
        assert r.returncode == 0 and r.stdout.startswith("gork "), (r.stdout, r.stderr)
        print("ok: starts with only Windows on the PATH:", r.stdout.strip())

        # the config where a Windows user would put it, written the
        # Windows way, key included
        with open(os.path.join(home, ".gork.conf"), "w") as f:
            f.write("relay = 127.0.0.1:%d\nhost = fake.local\n"
                    "api_key = sk-test\nretries = 0\n" % PORT)
        script[:] = [tool("write_file", path="out.txt", content="hello\n"),
                     tool("run_command", command="echo from sh && cmd /c ver"),
                     text("all done")]
        r = gork(["go"], home, work)
        assert "all done" in r.stdout, (r.stdout, r.stderr)
        write, run = results()
        assert "wrote 6 bytes" in write["content"], write
        assert "from sh" in run["content"] and "Windows" in run["content"], run
        assert run["content"].rstrip().endswith("[exit 0]"), run
        with open(os.path.join(work, "out.txt")) as f:
            assert f.read() == "hello\n"
        names = [t["name"] for t in requests[0]["tools"]]
        assert "run_command" in names, names
        print("ok: config in the profile, write_file, run_command through "
              "the package's sh")
        check_tui(home, work)
    srv.shutdown()
    print("all Cygwin package checks passed")


if __name__ == "__main__":
    main()
