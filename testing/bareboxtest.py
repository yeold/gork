#!/usr/bin/env python3
"""Run the `gork` command of bareboxbuild.sh's barebox, in QEMU.

Usage:  python3 testing/bareboxtest.py PACKAGE FAKEAPI

PACKAGE is gork-<version>-barebox-arm64.zip, FAKEAPI testing/fakeapi.c
built for this machine.  barebox boots in QEMU's arm64 virt machine
(barebox-dt-2nd.img as the kernel) with a virtio network card on QEMU's
user network, which takes it to the fake API on 10.0.2.2; the checks are
typed at barebox's prompt on the serial console, in /tmp (RAM), and the
files they leave are looked at there.  Prints "ok:" or "FAIL:" per check
and exits 1 if any failed.  Needs qemu-system-aarch64 and unzip.
"""
import json, os, re, select, shutil, subprocess, sys, tempfile, time, zipfile

PORT = 8099
PROMPT = b"virt64:/"


class Console:
    def __init__(self, cmd):
        self.p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        self.buf = b""
        self.all = b""

    def expect(self, pat, secs=120):
        end = time.time() + secs
        while time.time() < end:
            plain = re.sub(rb"\x1b\[[0-9;?]*[A-Za-z]", b"", self.buf)
            i = plain.find(pat)
            if i >= 0:
                self.buf = plain[i + len(pat):]
                return plain[:i + len(pat)].decode("utf-8", "replace")
            r, _, _ = select.select([self.p.stdout], [], [], 0.5)
            if r:
                d = os.read(self.p.stdout.fileno(), 65536)
                if not d:
                    break
                self.buf += d
                self.all += d
        raise TimeoutError("waiting for %r" % pat)

    def send(self, s):
        for c in s.encode():            # slowly: the serial line drops input
            self.p.stdin.write(bytes([c]))
            self.p.stdin.flush()
            time.sleep(0.01)

    def cmd(self, line, secs=120):
        """A command line at the prompt: what it printed."""
        self.send(line + "\r")
        out = self.expect(PROMPT, secs)
        self.expect(b" ", 5)
        return out.replace("\r", "")


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    pkg, fake = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
    t = tempfile.mkdtemp(prefix="bareboxtest.")
    with zipfile.ZipFile(pkg) as z:
        z.extract("barebox-dt-2nd.img", t)
    fp = subprocess.Popen([fake, str(PORT)], cwd=t, stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL)
    c = Console(["qemu-system-aarch64", "-M", "virt", "-cpu", "cortex-a57",
                 "-m", "1024", "-display", "none", "-serial", "stdio",
                 "-monitor", "none", "-kernel", os.path.join(t, "barebox-dt-2nd.img"),
                 "-netdev", "user,id=n0", "-device", "virtio-net-device,netdev=n0"])
    part = {}
    try:
        c.expect(b"to stop autoboot", 120)
        c.send(" ")
        c.expect(PROMPT, 30)
        c.expect(b" ", 5)
        c.cmd("echo -o /env/gork.cnf relay = 10.0.2.2:%d" % PORT)
        for line in ("api_key = sk-fake", "timeout = 20", "retries = 0"):
            c.cmd("echo -a /env/gork.cnf " + line)
        c.cmd("mkdir /tmp/w; mkdir /tmp/w/sub; cd /tmp/w")
        c.cmd("echo -o /tmp/w/abcdefghijkl.c old")
        part["version"] = c.cmd("gork --version")
        part["notui"] = c.cmd("gork")
        part["hello"] = c.cmd("gork hello", 240)
        c.cmd("GORK_ALLOW_RUN=1; export GORK_ALLOW_RUN")
        part["go"] = c.cmd("gork go", 300)
        c.cmd("GORK_ALLOW_RUN=")
        part["esc"] = c.cmd("gork esc", 240)
        c.cmd("cd /tmp/w/sub")
        part["sub"] = c.cmd("gork w1", 240)
        c.cmd("cd /tmp/w")
        part["slow"] = c.cmd("gork slow; echo ==status $?", 240)
        part["again"] = c.cmd("gork hello", 240)
        part["out"] = c.cmd("md5sum /tmp/w/out.txt")
        part["w1"] = c.cmd("cat /tmp/w/sub/w1.txt")
        part["escape"] = c.cmd("ls /tmp/escape.txt")
    except TimeoutError as e:
        print("bareboxtest: " + str(e))
    finally:
        c.p.kill()
        fp.terminate()
        fp.wait()
    out = c.all.decode("utf-8", "replace").replace("\r", "")
    if os.environ.get("BAREBOXTEST_VERBOSE"):
        print(out)

    log = ""
    if os.path.exists(os.path.join(t, "fakeapi.log")):
        with open(os.path.join(t, "fakeapi.log")) as f:
            log = f.read()
    results = []                        # each tool_result's text
    for line in log.splitlines():
        try:
            for m in json.loads(line).get("messages", [])[-1:]:
                for b in m["content"] if isinstance(m["content"], list) else []:
                    if b.get("type") == "tool_result":
                        r = b.get("content")
                        results.append(r if isinstance(r, str) else json.dumps(r))
        except (ValueError, KeyError, TypeError):
            pass
    p = lambda k: part.get(k, "")

    checks = [
        ("--version", "gork " in p("version")),
        ("batch mode only", "no TUI" in p("notui")),
        ("a reply, on gork's own TCP/IP", "PONG" in p("hello")),
        # md5 of "a\nb\n": barebox's files are as written, no CR LF
        ("write_file", "dd8c6a395b5dd36c56d23275028f526c" in p("out")),
        ("read_file", "a\nb\n" in results),
        ("list_dir", "abcdefghijkl.c" in log),
        ("run_command, its output taken from the console",
         any("Command not found" in r and "[exit " in r for r in results)),
        ("paths outside the working directory refused",
         "outside the working directory" in log and "No such file" in p("escape")),
        ("a write in a subdirectory", "\nx" in p("w1")),
        ("the request timeout", "timeout on socket" in p("slow")),
        ("exit status in $?", "==status 1" in p("slow")),
        ("a run after a failed one starts afresh", "PONG" in p("again")),
    ]
    fails = 0
    for name, good in checks:
        print(("ok: " if good else "FAIL: ") + name)
        fails += not good
    if fails and not os.environ.get("BAREBOXTEST_VERBOSE"):
        print(out[-3000:])
    shutil.rmtree(t, ignore_errors=True)
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
