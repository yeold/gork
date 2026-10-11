#!/usr/bin/env python3
"""Run gork.exe on FreeDOS in QEMU against a fake API and check it.

Usage:  python3 testing/dostest.py [gork-<version>-dos.zip]

The zip is what dosbuild.sh makes (default: the newest out/gork-*-dos.zip).
Needs qemu-system-i386, mtools and unzip.  The FreeDOS boot floppy and the
NE2000 packet driver are downloaded once into $GORK_VM/dostest (default
~/.cache/gork-vm/dostest).  The guest reaches the fake API on the host as
10.0.2.2; the keyboard is typed and the screen read through the QEMU
monitor.
"""
import glob, hashlib, http.server, json, os, re, shutil, socket, struct
import subprocess, sys, threading, time, urllib.request

VM = os.path.join(os.environ.get("GORK_VM",
                                 os.path.expanduser("~/.cache/gork-vm")),
                  "dostest")
PORT = 8095
FD = "https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files"
DOWNLOADS = [
    (FD + "/distributions/1.3/official/FD13-FloppyEdition.zip",
     "75a4e11a7fce6f124e20927b3022b4b715a2a3f7324c5f5bfea42d90d80eb072"),
    (FD + "/repositories/1.3/net/crynwr.zip",
     "fd18758586d3efa7cf9cd9a69f3b61a79f2d5d5f46a97cc781a66bd28a5b5ec2"),
]


def sh(*cmd, **kw):
    subprocess.run(cmd, check=True, **kw)


# ------------------------------------------------------------- fake API

script = []         # replies still to give, in order
requests = []       # every request body received


def tool(name, **inp):
    return {"id": "m", "stop_reason": "tool_use",
            "content": [{"type": "tool_use", "id": "t1", "name": name,
                         "input": inp}],
            "usage": {"input_tokens": 1000, "output_tokens": 20}}


def text(s, delay=0):
    return {"id": "m", "stop_reason": "end_turn", "delay": delay,
            "content": [{"type": "text", "text": s}],
            "usage": {"input_tokens": 1200, "output_tokens": 9}}


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def do_POST(self):
        n = int(self.headers.get("content-length", 0))
        requests.append(json.loads(self.rfile.read(n)))
        rep = dict(script.pop(0) if script else text("unscripted"))
        time.sleep(rep.pop("delay", 0))
        body = json.dumps(rep).encode()
        try:
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        except OSError:
            pass                        # the client aborted

    def log_message(self, *a):
        pass


def results():
    """The tool_result blocks gork has sent back, oldest first."""
    out = []
    for r in requests:
        last = r["messages"][-1]["content"]
        if isinstance(last, list):
            out += [b for b in last if b.get("type") == "tool_result"]
    return out


# ------------------------------------------------------------ the guest

def fetch():
    os.makedirs(VM, exist_ok=True)
    for url, digest in DOWNLOADS:
        path = os.path.join(VM, os.path.basename(url))
        if not os.path.exists(path):
            print("fetching", url)
            urllib.request.urlretrieve(url, path)
        with open(path, "rb") as f:
            assert hashlib.sha256(f.read()).hexdigest() == digest, path


def build_images(zip_path):
    """boot.img: the FreeDOS floppy with our startup files.  disk.img: C:,
    one FAT16 partition holding gork and a working directory C:\\W."""
    c = os.path.join(VM, "c")
    shutil.rmtree(c, ignore_errors=True)
    os.makedirs(os.path.join(c, "W", "_GORK", "SKILLS", "DEPLOY"))
    os.makedirs(os.path.join(c, "TMP"))
    sh("unzip", "-qo", zip_path, "gork.exe", "CWSDPMI.EXE", "-d", c)
    sh("unzip", "-qoj", os.path.join(VM, "crynwr.zip"),
       "DRIVERS/CRYNWR/NE2000.COM", "-d", c)
    files = {
        "WATTCP.CFG": "my_ip = 10.0.2.15\nnetmask = 255.255.255.0\n"
                      "gateway = 10.0.2.2\nnameserver = 10.0.2.3\n",
        "GORK.CNF": "relay = 10.0.2.2:%d\nhost = fake.local\n"
                    "timeout = 6\nretries = 0\n" % PORT,
        "SECRET.TXT": "secret\n",
        "W/GORK.MD": "Always answer in haiku.\n",
        "W/IN.TXT": "inside\n",
        "W/TWO.TXT": "one\ntwo\nthree\n",
        "W/_GORK/SKILLS/DEPLOY/SKILL.MD":
            "---\nname: deploy\ndescription: Ship it.\n---\nSteps.\n",
    }
    for name, body in files.items():
        with open(os.path.join(c, name), "w", newline="\r\n") as f:
            f.write(body)

    boot = os.path.join(VM, "boot.img")
    sh("unzip", "-qoj", os.path.join(VM, "FD13-FloppyEdition.zip"),
       "144m/x86BOOT.img", "-d", VM)
    shutil.copy(os.path.join(VM, "x86BOOT.img"), boot)
    startup = {
        "fdconfig.sys": "LASTDRIVE=Z\nBUFFERS=20\nFILES=40\n"
                        "SHELL=\\FREEDOS\\BIN\\COMMAND.COM \\FREEDOS\\BIN "
                        "/E:2048 /P=\\FDAUTO.BAT\n",
        "fdauto.bat": "@echo off\nset PATH=A:\\FREEDOS\\BIN;C:\\\n"
                      "set WATTCP.CFG=C:\\\nset TEMP=C:\\TMP\n"
                      "set GORK_ALLOW_RUN=1\nC:\nNE2000 0x60 5 0x300\n"
                      "cd W\necho READY\n",
    }
    for name, body in startup.items():
        path = os.path.join(VM, name)
        with open(path, "w", newline="\r\n") as f:
            f.write(body)
        sh("mcopy", "-o", "-i", boot, path, "::" + name)

    disk = os.path.join(VM, "disk.img")
    sectors, start = 64 * 2048, 2048
    with open(disk, "wb") as f:
        f.truncate(sectors * 512)
        f.seek(446)                     # one bootable FAT16 partition
        f.write(struct.pack("<B3sB3sII", 0x80, bytes([32, 33, 0]), 6,
                            bytes([254, 63, 7]), start, sectors - start))
        f.write(bytes(48) + b"\x55\xaa")
    part = "%s@@%d" % (disk, start * 512)
    sh("mformat", "-i", part, "-T", str(sectors - start), "-h", "16",
       "-s", "63", "-H", str(start), "::")
    sh("mcopy", "-s", "-i", part, *glob.glob(os.path.join(c, "*")), "::")
    return boot, disk


KEYS = {" ": "spc", "\n": "ret", "-": "minus", "=": "equal", ".": "dot",
        "/": "slash", ",": "comma", ";": "semicolon", "\\": "backslash",
        "\b": "backspace"}
SHIFT = {":": "semicolon", '"': "apostrophe", "_": "minus", ">": "dot",
         "<": "comma", "?": "slash", "*": "8"}


class Guest:
    def __init__(self, boot, disk):
        self.sock_path = os.path.join(VM, "mon.sock")
        if os.path.exists(self.sock_path):
            os.remove(self.sock_path)
        self.qemu = subprocess.Popen(
            ["qemu-system-i386", "-m", "32", "-display", "none", "-vga", "std",
             "-boot", "a", "-serial", "none",
             "-monitor", "unix:%s,server,nowait" % self.sock_path,
             "-drive", "file=%s,format=raw,if=floppy" % boot,
             "-drive", "file=%s,format=raw,if=ide,index=0" % disk,
             "-netdev", "user,id=n0",
             "-device", "ne2k_isa,netdev=n0,iobase=0x300,irq=5"])
        for _ in range(50):
            if os.path.exists(self.sock_path):
                break
            time.sleep(0.1)
        self.mon = socket.socket(socket.AF_UNIX)
        self.mon.connect(self.sock_path)
        self.cmd("")

    def cmd(self, c):
        self.mon.sendall(c.encode() + b"\n")
        out = b""
        self.mon.settimeout(2)
        while not out.rstrip().endswith(b"(qemu)"):
            try:
                d = self.mon.recv(65536)
            except socket.timeout:
                break
            if not d:
                break
            out += d
        return out.decode("latin1")

    def key(self, name):
        self.cmd("sendkey " + name)
        time.sleep(0.06)

    def type(self, s):
        for ch in s:
            if ch in KEYS:
                self.key(KEYS[ch])
            elif ch in SHIFT:
                self.key("shift-" + SHIFT[ch])
            elif ch.isupper():
                self.key("shift-" + ch.lower())
            else:
                self.key(ch)

    def screen(self):
        """The 80x25 text screen, blank lines dropped."""
        out = self.cmd("xp /2000hx 0xb8000")
        words = [int(x, 16) for line in out.splitlines() if ":" in line
                 for x in re.findall(r"0x([0-9a-f]{4})",
                                     line.split(":", 1)[1])]
        chars = "".join(chr(w & 0xff) if 32 <= (w & 0xff) < 127 else " "
                        for w in words)
        rows = [chars[i:i + 80].rstrip() for i in range(0, len(chars), 80)]
        return "\n".join(r for r in rows if r)

    def wait(self, pattern, secs=40):
        """Wait until the screen matches `pattern`; return the screen."""
        end = time.time() + secs
        while time.time() < end:
            s = self.screen()
            if re.search(pattern, s):
                return s
            time.sleep(0.5)
        raise AssertionError("no %r on screen:\n%s" % (pattern, self.screen()))

    def run(self, line, secs=60):
        """Type a DOS command and wait for the prompt to come back."""
        self.type("cls\n")
        self.wait(r"^C:\\W>$", 10)
        self.type(line + "\n")
        time.sleep(1)
        return self.wait(r"C:\\W>$", secs)

    def close(self):
        try:
            self.cmd("quit")
        except OSError:
            pass
        self.qemu.wait()


# -------------------------------------------------------------- checks

def check_batch(g):
    """A tool_use round trip; GORK.md and the skill index reach the model;
    write_file goes through its 8.3 temp name."""
    del requests[:]
    script[:] = [tool("write_file", path="out.txt", content="hello\n"),
                 text("wrote it")]
    s = g.run("..\\gork t")
    assert "wrote it" in s, s
    system = requests[0]["system"]
    assert system.startswith("Always answer in haiku."), system
    assert "deploy" in system and "Ship it." in system, system
    assert "wrote 6 bytes" in results()[0]["content"], results()
    assert "hello" in g.run("type out.txt")
    names = [t["name"] for t in requests[0]["tools"]]
    assert "run_command" in names, names
    print("ok: batch round trip, GORK.md, skills, write_file")


def check_confinement(g):
    """Paths outside C:\\W are refused however they are spelled."""
    outside = ["c:\\secret.txt", "..\\secret.txt", "C:/W/../SECRET.TXT",
               "\\secret.txt", "a:\\fdauto.bat"]
    inside = ["in.txt", "C:\\W\\IN.TXT"]
    for p in outside + inside:
        del requests[:]
        script[:] = [tool("read_file", path=p), text("done")]
        g.run("..\\gork t")
        r = results()[0]
        if p in outside:
            assert r.get("is_error") and "outside" in r["content"], (p, r)
        else:
            assert "inside" in r["content"], (p, r)
    print("ok: path confinement")


def check_edit(g):
    """edit_file matches across lines in a CR LF file, and keeps the CR LFs:
    the model only ever sees and sends bare newlines."""
    del requests[:]
    script[:] = [tool("read_file", path="two.txt"),
                 tool("edit_file", path="two.txt", old_text="one\ntwo\n",
                      new_text="one\n2\n"),
                 text("done")]
    g.run("..\\gork t")
    read, edit = results()
    assert read["content"] == "one\ntwo\nthree\n", read
    assert not edit.get("is_error"), edit
    print("ok: edit_file on a CR LF file")


def check_run(g):
    """run_command returns the output and the exit status."""
    del requests[:]
    script[:] = [tool("run_command", command="ver"), text("done")]
    g.run("..\\gork t")
    r = results()[0]
    assert "FreeCom" in r["content"] and "[exit 0]" in r["content"], r
    assert not r.get("is_error"), r
    print("ok: run_command")


def check_timeout(g):
    """A reply that never comes ends in a timeout, not a crash."""
    script[:] = [text("late", delay=12)]
    s = g.run("..\\gork t")
    assert "timeout on socket" in s, s
    time.sleep(7)                       # let the late reply drain
    print("ok: timeout")


def check_tui(g):
    """Trust prompt, Backspace, approval, PgUp/PgDn, Esc, a clean exit."""
    del requests[:]
    script[:] = [tool("write_file", path="tui.txt", content="x\n"),
                 text("all done"), text("never seen", delay=10)]
    g.type("cls\n")
    g.type("..\\gork\n")
    g.wait("Trust this directory")
    g.type("y\n")
    g.wait("Enter send")
    g.type("hix\b\n")
    g.wait("Allow write_file")
    g.type("y")
    g.wait("all done")
    assert requests[0]["messages"][0]["content"] == "hi", requests[0]
    g.key("pgup")
    g.key("pgdn")
    g.wait("Enter send")
    g.type("again\n")
    g.wait("Esc to abort")
    g.key("esc")
    g.wait("aborted")
    g.type("/quit\n")
    s = g.wait(r"C:\\W>$", 20)
    assert "Enter send" not in s, s     # the screen was cleared
    assert "TRUSTED" in g.run("dir /b c:\\_gork")
    assert "TUI.TXT" in g.run("dir /b").upper()
    time.sleep(10)
    print("ok: TUI")


def main():
    zips = sorted(glob.glob("out/gork-*-dos.zip"), key=os.path.getmtime)
    zip_path = sys.argv[1] if len(sys.argv) > 1 else (zips[-1] if zips else None)
    if zip_path is None:
        sys.exit("no gork-*-dos.zip; run `sh dosbuild.sh` first")
    fetch()
    boot, disk = build_images(os.path.abspath(zip_path))
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    g = Guest(boot, disk)
    try:
        g.wait(r"C:\\W>$", 90)
        check_batch(g)
        check_confinement(g)
        check_edit(g)
        check_run(g)
        check_timeout(g)
        check_tui(g)
    finally:
        g.close()
        srv.shutdown()
    # CR LF must have survived the edit: read the file back off the disk
    out = subprocess.run(["mtype", "-i", "%s@@%d" % (disk, 2048 * 512),
                          "::W/TWO.TXT"], capture_output=True, check=True)
    assert out.stdout == b"one\r\n2\r\nthree\r\n", out.stdout
    print("all DOS checks passed")


if __name__ == "__main__":
    main()
