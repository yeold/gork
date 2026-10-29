"""Checks for a gork built --with-serial: all its traffic over a serial
line, in the frames gork-relay -S takes (docs/SERIAL.md).

Usage:  python3 testing/serialtest.py ./gork
Exits 0 only if every check passes.

The line is a pseudo-terminal.  This script is the relay at its other end,
and the API behind it: it reads gork's frames and answers them itself, so
that it can also send what a real relay would not (frames left over from
an earlier request, noise, a damaged frame).  gork-relay's own side of
the line is checked in its test/relaytest.py.
"""
import json, os, select, subprocess, sys, tempfile, threading, time

BINARY = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "./gork")
END, ESC = 0xC0, 0xDB


def fletcher(b):
    x = y = 0
    for c in b:
        x = (x + c) % 255
        y = (y + x) % 255
    return y << 8 | x


def frame(t, i, d=b"", damage=False):
    raw = bytes([ord(t), i]) + d
    s = fletcher(raw) ^ damage
    raw += bytes([s >> 8, s & 255])
    raw = raw.replace(b"\xdb", b"\xdb\xdd").replace(b"\xc0", b"\xdb\xdc")
    return b"\xc0" + raw + b"\xc0"


def message(text, stop="end_turn", content=None):
    body = json.dumps({"id": "m", "type": "message", "role": "assistant",
                       "stop_reason": stop,
                       "content": content or [{"type": "text", "text": text}],
                       "usage": {"input_tokens": 9, "output_tokens": 3}},
                      ensure_ascii=False).encode()
    return (b"HTTP/1.0 200 OK\r\nContent-Type: application/json\r\n"
            b"Content-Length: %d\r\n\r\n" % len(body)) + body


class Relay:
    """The far end of the line.  `answer(body)` gives the bytes to send
    back for a request (None: send nothing), `before` what to send ahead
    of them; requests and the ids that were closed are kept."""

    def __init__(self, fd):
        self.fd, self.buf = fd, b""
        self.requests, self.closed = [], []
        self.answer, self.before, self.damage = None, b"", False
        threading.Thread(target=self.run, daemon=True).start()

    def frames(self):
        while True:
            r, _, _ = select.select([self.fd], [], [], 0.2)
            if r:
                try:
                    self.buf += os.read(self.fd, 65536)
                except OSError:             # no one has the line open
                    time.sleep(0.1)
            while b"\xc0" in self.buf:
                f, _, self.buf = self.buf.partition(b"\xc0")
                f = f.replace(b"\xdb\xdc", b"\xc0").replace(b"\xdb\xdd", b"\xdb")
                if len(f) >= 4:
                    assert fletcher(f[:-2]) == f[-2] << 8 | f[-1], f[:40]
                    yield chr(f[0]), f[1], f[2:-2]

    def run(self):
        cur, req = None, b""
        for t, i, d in self.frames():
            if t == "O":
                cur, req = i, b""
            elif t == "C":
                self.closed.append(i)
            elif t == "D" and i == cur:
                req += d
                head, sep, body = req.partition(b"\r\n\r\n")
                n = [int(l.split(b":")[1]) for l in head.split(b"\r\n")
                     if l.lower().startswith(b"content-length:")]
                if sep and n and len(body) == n[0]:
                    self.requests.append((i, body))
                    out = self.answer(body)
                    if out is None:
                        continue
                    data = b"".join(frame("D", i, out[k:k + 1024])
                                    for k in range(0, len(out), 1024))
                    if self.damage:
                        data = frame("D", i, out[:100]) + \
                            frame("D", i, out[100:200], damage=True)
                    os.write(self.fd, self.before + data + frame("C", i))


def run(tmp, dev, task="hi", config=None, **env_extra):
    env = dict(os.environ, HOME=tmp, ANTHROPIC_API_KEY="sk-test")
    env.update(env_extra)
    conf = os.path.join(tmp, "gork.conf")
    with open(conf, "w") as f:
        f.write(config if config is not None else
                "relay = serial:%s:115200\nretries = 0\ntimeout = 10\n" % dev)
    os.chmod(conf, 0o600)
    env["GORK_CONFIG"] = conf
    return subprocess.run([BINARY, task], cwd=os.path.join(tmp, "w"),
                          capture_output=True, text=True, timeout=30, env=env)


def main():
    master, slave = os.openpty()
    dev = os.ttyname(slave)
    relay = Relay(master)
    with tempfile.TemporaryDirectory() as tmp:
        os.mkdir(os.path.join(tmp, "w"))
        with open(os.path.join(tmp, "w", "seen.txt"), "w") as f:
            f.write("a file")

        # a tool round trip: two requests over the line
        def tool_loop(body):
            if b"tool_result" in body:
                return message("done after the tool")
            return message("", "tool_use", [{"type": "tool_use", "id": "t1",
                                             "name": "list_dir",
                                             "input": {"path": "."}}])
        relay.answer = tool_loop
        p = run(tmp, dev)
        assert p.returncode == 0 and "done after the tool" in p.stdout, \
            (p.stdout, p.stderr)
        assert b"seen.txt" in relay.requests[-1][1]
        print("ok: a tool round trip over the serial line")

        # END and ESC bytes both ways: U+06C0 is DB 80 in UTF-8
        big = "ۀఀ big " * 5000
        with open(os.path.join(tmp, "w", "GORK.md"), "w") as f:
            f.write("rules ۀ" * 1000)
        relay.answer = lambda body: message(big)
        p = run(tmp, dev)
        os.remove(os.path.join(tmp, "w", "GORK.md"))
        assert p.returncode == 0 and p.stdout == big + "\n", \
            (len(p.stdout), p.stderr)
        assert ("rules ۀ" * 1000).encode() in relay.requests[-1][1]
        print("ok: a reply of %d frames and a request of %d, with END and "
              "ESC bytes in both" % (len(big.encode()) // 1024 + 1,
                                     len(relay.requests[-1][1]) // 1024 + 1))

        # what an abandoned request leaves on the line is not taken for
        # the reply
        last = relay.requests[-1][0]
        relay.before = (frame("D", last, b"x" * 500)[:300] +
                        b"\xc0\x44\x07\x09\xc0\xdbjunk\xc0" +
                        frame("D", (last + 1) & 255, b"ours, cut")[:6])
        relay.answer = lambda body: message("the right reply")
        p = run(tmp, dev)
        relay.before = b""
        assert p.returncode == 0 and p.stdout == "the right reply\n", \
            (p.stdout, p.stderr)
        print("ok: an earlier request's frames, and noise, ignored")

        relay.damage = True
        p = run(tmp, dev)
        relay.damage = False
        assert p.returncode != 0 and "damaged" in p.stderr, p.stderr
        print("ok: a damaged frame reported")

        # no reply: gork times out and tells the relay it gave up
        relay.answer = lambda body: None
        p = run(tmp, dev, config="relay = serial:%s\nretries = 0\n"
                "timeout = 2\n" % dev)
        time.sleep(0.5)
        assert p.returncode != 0 and "timeout" in p.stderr, p.stderr
        assert relay.closed and relay.closed[-1] == relay.requests[-1][0], \
            (relay.closed, relay.requests[-1][0])
        relay.answer = lambda body: message("and the next is fine")
        p = run(tmp, dev)
        assert p.returncode == 0 and "the next is fine" in p.stdout, p.stderr
        print("ok: a timeout closes the request, and the next one works")

        p = run(tmp, dev, config="relay = 127.0.0.1:8443\n")
        assert p.returncode == 2 and "serial" in p.stderr, p.stderr
        p = run(tmp, dev, config="relay = serial:%s:12345\n" % dev)
        assert p.returncode != 0 and "speed" in p.stderr, p.stderr
        print("ok: a network relay, and a speed there is none of, refused")
    print("all serial checks passed")


if __name__ == "__main__":
    main()
