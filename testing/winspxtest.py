#!/usr/bin/env python3
"""Check SPX from Windows: gork.exe on Windows 95 to Me in QEMU, reaching a
fake API through gork-relay.

Usage:  GORK_WIN_DISK=c.img GORK_RELAY=gork-relay \\
            python3 testing/winspxtest.py gork-<v>-win32-legacy.zip [EXE]

GORK_WIN_DISK is the hard disk image of an installed Windows with a network
card QEMU has (ne2k_pci), the "IPX/SPX-compatible Protocol" added in the
Network control panel, and nothing asked at start-up but the network
password.  It runs as a snapshot and is not changed, but for one batch
file put in its StartUp folder the first time, which runs A:\\GO.BAT if
there is one.  A floppy carries gork in and the results out.  Needs
qemu-system-i386, mtools and unzip; takes a quarter of an hour.  Exits 0
if the round trip worked.
"""
import http.server, json, os, socket, subprocess, sys, threading, time

import struct, tempfile

DISK, RELAY, PORT = os.environ["GORK_WIN_DISK"], os.environ["GORK_RELAY"], 8098
ZIP = os.path.abspath(sys.argv[1])
TMP = tempfile.mkdtemp()
MON = os.path.join(TMP, "mon")
EXE = sys.argv[2] if len(sys.argv) > 2 else "gork.exe"
requests = []
script = [
    {"id": "m", "stop_reason": "tool_use", "content": [
        {"type": "tool_use", "id": "t1", "name": "write_file",
         "input": {"path": "spx.txt", "content": "over spx\n"}}]},
    {"id": "m", "stop_reason": "end_turn",
     "content": [{"type": "text", "text": "spx all done"}]}]


class H(http.server.BaseHTTPRequestHandler):
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


def sh(*a, **k):
    return subprocess.run(a, check=True, capture_output=True, text=True, **k)


def key(name):
    """A key to the guest, through the QEMU monitor."""
    try:
        m = socket.socket(socket.AF_UNIX)
        m.connect(MON)
        time.sleep(0.2)
        m.sendall(b"sendkey %s\n" % name.encode())
        time.sleep(0.2)
        m.close()
    except OSError:
        pass                    # not up yet


def main():
    env = dict(os.environ, MTOOLS_SKIP_CHECK="1")
    with open(DISK, "rb") as f:         # the first partition of the disk
        start = struct.unpack("<I", f.read(512)[454:458])[0]
    c = "%s@@%d" % (DISK, start * 512)
    startup = "::/WINDOWS/Start Menu/Programs/StartUp/GORKTEST.BAT"
    if subprocess.run(["mdir", "-i", c, startup], env=env,
                      capture_output=True).returncode != 0:
        with open(TMP + "/gorktest.bat", "w", newline="\r\n") as f:
            f.write("@echo off\nif exist a:\\go.bat call a:\\go.bat\n")
        sh("mcopy", "-i", c, TMP + "/gorktest.bat", startup, env=env)

    fl = TMP + "/a.img"
    sh("dd", "if=/dev/zero", "of=" + fl, "bs=1474560", "count=1")
    sh("mformat", "-i", fl, "-f", "1440", "::")
    sh("unzip", "-qoj", ZIP, EXE, "-d", TMP)
    files = {
        "SPX.CNF": "relay = spx:1.02676F726B01:8443\nhost = fake.local\n"
                   "api_key = sk-test\ntimeout = 60\nretries = 0\n",
        "GO.BAT": "@echo off\nmkdir c:\\spxwork\ncopy a:\\gork.exe c:\\spxwork\n"
                  "set GORK_CONFIG=a:\\spx.cnf\nc:\ncd \\spxwork\n"
                  "gork.exe go > a:\\out.txt\ncopy spx.txt a:\\\n"
                  "echo finished > a:\\done.txt\n"}
    for name, text in files.items():
        with open(TMP + "/" + name, "w", newline="\r\n") as f:
            f.write(text)
    sh("mcopy", "-i", fl, TMP + "/" + EXE, "::GORK.EXE")
    for name in files:
        sh("mcopy", "-i", fl, TMP + "/" + name, "::" + name)

    srv = http.server.ThreadingHTTPServer(("127.0.0.1", PORT), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    log = open(TMP + "/relay.log", "w")
    relay = subprocess.Popen([RELAY, "-v", "-p", "-n", "1", "-t", "0", "-x",
                              "udp:127.0.0.1:8097", "127.0.0.1:%d" % PORT],
                             stdout=log, stderr=log)
    time.sleep(0.3)
    assert relay.poll() is None, "gork-relay did not start"
    qemu = subprocess.Popen(
        ["qemu-system-i386", "-M", "pc", "-cpu", "pentium2", "-m", "128",
         "-vga", "cirrus", "-display", "none", "-rtc", "base=localtime",
         "-monitor", "unix:%s,server,nowait" % MON, "-serial", "none",
         "-drive", "file=%s,format=raw,if=floppy" % fl,
         "-drive", "file=%s,format=raw,if=ide,index=0,snapshot=on" % DISK,
         "-netdev", "user,id=u0",
         "-netdev", "socket,id=s0,udp=127.0.0.1:8097,localaddr=127.0.0.1:8099",
         "-netdev", "hubport,id=h0,hubid=0,netdev=u0",
         "-netdev", "hubport,id=h1,hubid=0,netdev=s0",
         "-netdev", "hubport,id=n0,hubid=0", "-device", "ne2k_pci,netdev=n0",
         "-no-reboot", "-icount", "shift=3", "-boot", "order=c"])
    try:
        end = time.time() + 1500
        while time.time() < end:
            time.sleep(10)
            assert qemu.poll() is None, "QEMU ended"
            key("ret")          # "Enter Network Password": OK
            if subprocess.run(["mdir", "-i", fl, "::DONE.TXT"], env=env,
                              capture_output=True).returncode == 0:
                break
        else:
            raise AssertionError("no DONE.TXT after 25 minutes")
        time.sleep(5)
        out = sh("mtype", "-i", fl, "::OUT.TXT", env=env).stdout
        made = sh("mtype", "-i", fl, "::SPX.TXT", env=env).stdout
    finally:
        qemu.kill()
        relay.kill()
        srv.shutdown()
    assert "spx all done" in out, out
    assert made.replace("\r", "") == "over spx\n", made
    assert len(requests) == 2, len(requests)
    assert "wrote" in json.dumps(requests[1]["messages"][-1]), requests[1]
    assert "opened" in open(TMP + "/relay.log").read()
    print("ok: %s over SPX through gork-relay" % EXE)


main()
