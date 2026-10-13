#!/usr/bin/env python3
"""Run gork.exe on FreeDOS in QEMU against a fake API and check it.

Usage:  python3 testing/dostest.py [gork-<version>-dos.zip]

The zip is what dosbuild.sh makes (default: the newest out/gork-*-dos.zip).
Needs qemu-system-i386, mtools and unzip.  The FreeDOS boot floppy and the
NE2000 packet driver are downloaded once into $GORK_VM/dostest (default
~/.cache/gork-vm/dostest).  To test on another DOS, name a bootable 1.44 MB
floppy image of it in $GORK_DOS_FLOPPY (it needs COMMAND.COM in its root
and a little free space); MS-DOS 6.22 install disk 1 works.  Set
$GORK_DOS_ONE_DISK to skip the check that needs a D: drive (DR-DOS 7.03's
install floppy does not see the second disk).  To run the checks in a DOS
box under Windows 3.0 or 3.1 in 386 enhanced mode, name an installed
WINDOWS directory, copied off a disk where Setup ran (VGA, no network,
with its HIMEM.SYS), in $GORK_DOS_WINDOWS, together with $GORK_DOS_FLOPPY:
Windows is started from the floppy's DOS with a full-screen COMMAND.COM,
and the packet driver is the real-mode one loaded before it, with WINPKT.
A WINDOWS directory of Windows 1 or 2 (it has WIN100.BIN or WIN200.BIN) is
run in real mode instead, and needs a DOS of its time as $GORK_DOS_FLOPPY,
with $GORK_DOS_SMALL_DISKS: MS-DOS 4.01 (its "Setup" disk) for Windows 2,
which runs no DOS program on MS-DOS 6, and MS-DOS 3.30 for Windows 1,
which hangs on 4.01.
Name a gork-relay binary (https://github.com/yeold/gork-relay) in
$GORK_RELAY to check SPX as well: the guest then loads PDIPX, Novell's
IPX and SPX on the packet driver, and its network card is also wired to
that relay, which passes what it gets on to the fake API.
Set $GORK_DOS_SMALL_DISKS for MS-DOS 3.30, 4.01 and the like, which read
only disks laid out the old way.  The guest reaches the fake API on the host as
10.0.2.2; the keyboard is typed and the screen read through the QEMU
monitor.
"""
import glob, hashlib, http.server, json, os, re, shutil, socket, struct
import subprocess, sys, threading, time, urllib.request, zipfile

VM = os.path.join(os.environ.get("GORK_VM",
                                 os.path.expanduser("~/.cache/gork-vm")),
                  "dostest")
PORT = 8095
RELAY = os.environ.get("GORK_RELAY")    # a gork-relay binary: check SPX too
RELAY_PORT = 8097                       # where it takes the guest's frames
SMALL = bool(os.environ.get("GORK_DOS_SMALL_DISKS"))
WINDOWS = os.environ.get("GORK_DOS_WINDOWS")
# Windows 1 or 2: real mode
WINDOWS2 = bool(WINDOWS and any(os.path.exists(os.path.join(WINDOWS, n))
                                for n in ("WIN100.BIN", "WIN200.BIN")))
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


def wbox(c, shell):
    """C:\\WBOX.BAT, what Windows runs: READY, then a shell in C:\\W."""
    with open(os.path.join(c, "WBOX.BAT"), "w", newline="\r\n") as f:
        f.write("@echo off\nC:\ncd \\W\necho READY\n%s\n" % shell)


def windows2(c, windows, floppy):
    """Put Windows 1 or 2 on C: so that it starts a full-screen DOS box with
    WBOX.BAT in it; return the CONFIG.SYS lines it needs.

    Windows 2 runs every DOS program by its PIF, and one without a PIF in
    a window with 52K.  COMMAND.COM it always runs in a window, whatever
    COMMAND.PIF says, and there each program started is looked up again.
    So the shell is a copy of COMMAND.COM under another name, GSHELL.COM,
    with a PIF made from WORD.PIF (full screen), asking for 400K; what it
    starts runs inside its box.  run= in WIN.INI starts it.  CWSDPMI must
    be resident before Windows: loaded inside the box, it hangs."""
    shutil.copytree(windows, os.path.join(c, "WINDOWS"))
    wbox(c, "A:\\COMMAND.COM")
    sh("mcopy", "-n", "-i", floppy, "::COMMAND.COM",
       os.path.join(c, "GSHELL.COM"))
    with open(os.path.join(windows, "PIF", "WORD.PIF"), "rb") as f:
        pif = bytearray(f.read())

    def put(off, size, text):
        pif[off:off + size] = (text.encode() + b"\0").ljust(size, b" ")
    pif[2:32] = b"GSHELL".ljust(30)             # the window title
    pif[0x20:0x24] = struct.pack("<HH", 640, 400)   # KB desired, required
    put(0x24, 63, "C:\\GSHELL.COM")
    put(0x65, 64, "C:\\W")                       # start-up directory
    put(0xa5, 64, "/C C:\\WBOX.BAT")             # parameters
    pif[1] = sum(pif[2:0x171]) & 255            # checksum
    with open(os.path.join(c, "WINDOWS", "GSHELL.PIF"), "wb") as f:
        f.write(pif)
    ini = os.path.join(c, "WINDOWS", "WIN.INI")
    with open(ini, "rb") as f:
        text = f.read()
    assert b"\r\nrun=\r\n" in text, "no empty run= in WIN.INI"
    with open(ini, "wb") as f:
        f.write(text.replace(b"\r\nrun=\r\n",
                             b"\r\nrun=C:\\GSHELL.COM\r\n"))
    # HIMEM.SYS if the DOS disk has one (MS-DOS 4.01 does, 3.30 does not)
    has = subprocess.run(["mdir", "-i", floppy, "::HIMEM.SYS"],
                         capture_output=True).returncode == 0
    return "DEVICE=A:\\HIMEM.SYS\n" if has else ""


def build_images(zip_path):
    """boot.img: the FreeDOS floppy with our startup files.  disk.img: C:,
    one FAT16 partition holding gork and a working directory C:\\W."""
    c = os.path.join(VM, "c")
    shutil.rmtree(c, ignore_errors=True)
    os.makedirs(os.path.join(c, "W", "_GORK", "SKILLS", "DEPLOY"))
    os.makedirs(os.path.join(c, "TMP"))
    # the 16-bit zip (dos16build.sh) has no CWSDPMI: it needs none
    dpmi = "CWSDPMI.EXE" in zipfile.ZipFile(zip_path).namelist()
    sh("unzip", "-qo", zip_path, "gork.exe", *(["CWSDPMI.EXE"] if dpmi else []),
       "-d", c)
    sh("unzip", "-qoj", os.path.join(VM, "crynwr.zip"),
       "DRIVERS/CRYNWR/NE2000.COM", "-d", c)
    files = {
        "WATTCP.CFG": "my_ip = 10.0.2.15\nnetmask = 255.255.255.0\n"
                      "gateway = 10.0.2.2\nnameserver = 10.0.2.3\n",
        "GORK.CNF": "relay = 10.0.2.2:%d\nhost = fake.local\n"
                    "timeout = 6\nretries = 0\n"
                    "allow = %sc:/tmp\n"
                    % (PORT, "" if os.environ.get("GORK_DOS_ONE_DISK")
                       else "d:/shared;"),
        "BAD.CNF": "relay = 10.0.2.2:%d\nhost = fake.local\n"
                   "allow = c:/nope\n" % PORT,
        # the relay's address, as it prints it: network 0 is this segment
        "SPX.CNF": "relay = spx:0.02676F726B01:8443\nhost = fake.local\n"
                   "timeout = 20\nretries = 0\n",
        "SECRET.TXT": "secret\n",
        "W/GORK.MD": "Always answer in haiku.\n",
        "W/IN.TXT": "inside\n",
        "W/BIG.TXT": "0123456789abcdef0123456789abcdef0123456789abcdef\n"
                     * 2000,
        "W/TWO.TXT": "one\ntwo\nthree\n",
        "W/_GORK/SKILLS/DEPLOY/SKILL.MD":
            "---\nname: deploy\ndescription: Ship it.\n---\nSteps.\n",
    }
    for name, body in files.items():
        with open(os.path.join(c, name), "w", newline="\r\n") as f:
            f.write(body)

    boot = os.path.join(VM, "boot.img")
    rest = ("set WATTCP.CFG=C:\\\nset TEMP=C:\\TMP\nset GORK_ALLOW_RUN=1\n"
            "C:\nNE2000 0x60 5 0x300\ncd W\necho READY\n")
    if RELAY:
        sh("unzip", "-qoj", os.path.join(VM, "crynwr.zip"),
           "DRIVERS/CRYNWR/PDIPX.COM", "-d", c)
        rest = rest.replace("0x300\n", "0x300\nPDIPX\n")
    other = os.environ.get("GORK_DOS_FLOPPY")
    windows = WINDOWS
    himem = ""
    if WINDOWS2:
        himem = windows2(c, windows, other)
        rest = rest.replace("echo READY\n",
                            ("C:\\CWSDPMI -p\n" if dpmi else "") +
                            "cd \\WINDOWS\n%s\n" %
                            # Windows/386 is started with WIN386
                            ("WIN386" if os.path.exists(
                                os.path.join(windows, "WIN386.EXE")) else "WIN"))
    elif windows:                       # the checks run in a DOS box
        shutil.copytree(windows, os.path.join(c, "WINDOWS"))
        wbox(c, "A:\\COMMAND.COM")
        # WINPKT hands each received packet to the virtual machine that
        # asked for it; without it gork dies at its first disk access.
        sh("unzip", "-qoj", os.path.join(VM, "crynwr.zip"),
           "DRIVERS/CRYNWR/WINPKT.COM", "-d", c)
        rest = rest.replace("0x300\n", "0x300\nWINPKT 0x60\n")
        rest = rest.replace("echo READY\n", "C:\\WINDOWS\\WIN.COM /3 C:\\WBOX.BAT\n")
        assert os.path.exists(os.path.join(windows, "HIMEM.SYS")), \
            "no HIMEM.SYS in the WINDOWS directory; enhanced mode needs it"
        # /W: Windows 3.0 otherwise refuses enhanced mode on MS-DOS 5 and
        # later unless WINA20.386 is in the boot drive's root, where the
        # floppy has no room.  With DOS not in the HMA it is not needed.
        himem = "DEVICE=C:\\WINDOWS\\HIMEM.SYS\nSWITCHES=/W\n"
    if other:                           # e.g. MS-DOS 6.22 install disk 1
        shutil.copy(other, boot)
        startup = {
            "config.sys": himem + "BUFFERS=20\nFILES=40\n"
                          "SHELL=A:\\COMMAND.COM A:\\ /E:1024 /P\n",
            "autoexec.bat": "@echo off\nprompt $p$g\nset PATH=A:\\;C:\\\n"
                            + rest,
        }
    else:
        sh("unzip", "-qoj", os.path.join(VM, "FD13-FloppyEdition.zip"),
           "144m/x86BOOT.img", "-d", VM)
        shutil.copy(os.path.join(VM, "x86BOOT.img"), boot)
        startup = {
            "fdconfig.sys": "LASTDRIVE=Z\nBUFFERS=20\nFILES=40\n"
                            "SHELL=\\FREEDOS\\BIN\\COMMAND.COM "
                            "\\FREEDOS\\BIN /E:2048 /P=\\FDAUTO.BAT\n",
            "fdauto.bat": "@echo off\nset PATH=A:\\FREEDOS\\BIN;C:\\\n"
                          + rest,
        }
    for name, body in startup.items():
        path = os.path.join(VM, name)
        with open(path, "w", newline="\r\n") as f:
            f.write(body)
        sh("mcopy", "-o", "-i", boot, path, "::" + name)

    def fat_disk(name, mb, tree, active=0x80):
        """A disk image with one bootable FAT16 partition holding `tree`."""
        disk = os.path.join(VM, name)
        sectors, start, kind, more = mb * 2048, 2048, 6, []
        if SMALL:
            # What MS-DOS 4.01 reads: under 32 MB (type 4), starting on
            # the second track, 2K clusters.
            sectors, start, kind = min(mb * 2048 // 1008, 65) * 1008, 63, 4
            more = ["-c", "4"]
        with open(disk, "wb") as f:
            f.truncate(sectors * 512)
            f.seek(446)
            # CHS for the 16 heads x 63 sectors the BIOS and mformat use;
            # DR-DOS goes by these, not by the LBA fields.
            def chs(lba):
                c, r = divmod(lba, 16 * 63)
                return bytes([r // 63, r % 63 + 1 | (c >> 8) << 6, c & 255])
            f.write(struct.pack("<B3sB3sII", active, chs(start), kind,
                                chs(sectors - 1), start, sectors - start))
            f.write(bytes(48) + b"\x55\xaa")
        part = "%s@@%d" % (disk, start * 512)
        sh("mformat", "-i", part, "-T", str(sectors - start), "-h", "16",
           "-s", "63", "-H", str(start), *more, "::")
        if SMALL:
            # With mtools' name in the boot sector MS-DOS 4.01 does not
            # believe the rest of it, and shows an empty root directory.
            with open(disk, "r+b") as f:
                f.seek(start * 512 + 3)
                f.write(b"MSDOS4.0")
        sh("mcopy", "-s", "-i", part, *glob.glob(os.path.join(tree, "*")),
           "::")
        return disk

    d = os.path.join(VM, "d")           # D:, for the allow directories
    shutil.rmtree(d, ignore_errors=True)
    for name in ("SHARED/OK.TXT", "OTHER/NO.TXT"):
        os.makedirs(os.path.join(d, os.path.dirname(name)))
        with open(os.path.join(d, name), "w", newline="\r\n") as f:
            f.write("on d\n")
    disk = fat_disk("disk.img", 64, c)
    disk2 = fat_disk("disk2.img", 40, d, 0)    # type 6: 32 MB or more,
                                               # unless the disks are small
    return boot, disk, disk2


KEYS = {" ": "spc", "\n": "ret", "-": "minus", "=": "equal", ".": "dot",
        "/": "slash", ",": "comma", ";": "semicolon", "\\": "backslash",
        "\b": "backspace"}
SHIFT = {":": "semicolon", '"': "apostrophe", "_": "minus", ">": "dot",
         "<": "comma", "?": "slash", "*": "8"}


class Guest:
    def __init__(self, boot, disk, disk2):
        self.sock_path = os.path.join(VM, "mon.sock")
        if os.path.exists(self.sock_path):
            os.remove(self.sock_path)
        net = ["-netdev", "user,id=n0"]
        if RELAY:
            # the card on a hub with both the user network and the relay
            net = ["-netdev", "user,id=u0",
                   "-netdev", "socket,id=s0,udp=127.0.0.1:%d,"
                   "localaddr=127.0.0.1:%d" % (RELAY_PORT, RELAY_PORT + 1),
                   "-netdev", "hubport,id=h0,hubid=0,netdev=u0",
                   "-netdev", "hubport,id=h1,hubid=0,netdev=s0",
                   "-netdev", "hubport,id=n0,hubid=0"]
        self.qemu = subprocess.Popen(
            ["qemu-system-i386", "-m", "32", "-display", "none", "-vga", "std",
             "-boot", "a", "-serial", "none",
             "-monitor", "unix:%s,server,nowait" % self.sock_path,
             "-drive", "file=%s,format=raw,if=floppy" % boot,
             "-drive", "file=%s,format=raw,if=ide,index=0" % disk,
             "-drive", "file=%s,format=raw,if=ide,index=1" % disk2] + net +
            ["-device", "ne2k_isa,netdev=n0,iobase=0x300,irq=5"])
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
        # Windows 2 loses keys typed faster
        time.sleep(0.25 if WINDOWS2 else 0.06)

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
        # A DOS box under Windows echoes late: see the command on the
        # screen before Enter, or the bare prompt passes for its end.
        self.type(line)
        self.wait(re.escape(line[-8:]) + "$", 20)
        self.type("\n")
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


def check_bad_allow(g):
    """An allow directory that does not exist stops gork, as on Unix."""
    g.run("set GORK_CONFIG=c:\\bad.cnf")
    s = g.run("..\\gork t")
    g.run("set GORK_CONFIG=")
    assert "allow: c:/nope" in s, s
    print("ok: missing allow directory refused")


def check_allow(g):
    """An allow directory on another drive is reachable, its sibling is not;
    the list is ';'-separated, since ':' belongs to the drive letter."""
    cases = [("read_file", {"path": "d:\\shared\\ok.txt"}, "on d"),
             ("write_file", {"path": "D:/SHARED/NEW.TXT", "content": "n\n"},
              "wrote"),
             ("read_file", {"path": "d:\\other\\no.txt"}, "outside"),
             ("read_file", {"path": "d:\\shared\\..\\other\\no.txt"},
              "outside")]
    for name, inp, want in cases:
        del requests[:]
        script[:] = [tool(name, **inp), text("done")]
        g.run("..\\gork t")
        r = results()[0]
        assert want in r["content"], (inp, r)
        assert bool(r.get("is_error")) == (want == "outside"), (inp, r)
    print("ok: allow directory on a second drive")


def check_long(g):
    """Output past the cap is cut off and says so; a file too big to inline
    is refused."""
    del requests[:]
    script[:] = [tool("run_command", command="type big.txt"),
                 tool("read_file", path="big.txt"), text("done")]
    g.run("..\\gork t", 120)
    run, read = results()
    assert "[output truncated]" in run["content"], run["content"][-200:]
    assert len(run["content"]) < 70000, len(run["content"])
    assert read.get("is_error"), read["content"][:200]
    print("ok: long output")


def check_run(g):
    """run_command returns the output and the exit status."""
    del requests[:]
    script[:] = [tool("run_command", command="ver"), text("done")]
    g.run("..\\gork t")
    r = results()[0]
    out = r["content"]              # wording differs from DOS to DOS
    assert out.endswith("[exit 0]") and len(out.strip()) > 20, r
    assert not r.get("is_error"), r
    print("ok: run_command")


def check_spx(g):
    """A round trip over SPX to gork-relay, with a reply of many packets;
    then the TCP relay again, since both go through one packet driver."""
    del requests[:]
    big = "0123456789abcdef" * 1500
    script[:] = [tool("write_file", path="spx.txt", content="over spx\n"),
                 text(big + " spx done")]
    g.run("set GORK_CONFIG=c:\\spx.cnf")
    s = g.run("..\\gork t", 120)
    g.run("set GORK_CONFIG=")
    assert "spx done" in s, s
    assert "wrote 9 bytes" in results()[0]["content"], results()
    assert "over spx" in g.run("type spx.txt")
    del requests[:]
    script[:] = [text("tcp again")]
    assert "tcp again" in g.run("..\\gork t")
    print("ok: SPX through gork-relay")


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
    assert "TRUSTED" in g.run("dir c:\\_gork").upper()
    assert "TUI" in g.run("dir tui.*").upper()
    time.sleep(10)
    print("ok: TUI")


def main():
    zips = sorted(glob.glob("out/gork-*-dos.zip"), key=os.path.getmtime)
    zip_path = sys.argv[1] if len(sys.argv) > 1 else (zips[-1] if zips else None)
    if zip_path is None:
        sys.exit("no gork-*-dos.zip; run `sh dosbuild.sh` first")
    fetch()
    boot, disk, disk2 = build_images(os.path.abspath(zip_path))
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    relay = RELAY and subprocess.Popen(
        [RELAY, "-p", "-t", "0", "-x", "udp:127.0.0.1:%d" % RELAY_PORT,
         "127.0.0.1:%d" % PORT])
    if relay:
        time.sleep(0.3)
        assert relay.poll() is None, "gork-relay did not start"
    g = Guest(boot, disk, disk2)
    try:
        if WINDOWS2:
            # "Memory Configuration Altered", OK: Enter until the box is up
            for _ in range(12):
                time.sleep(10)
                if "READY" in g.screen():
                    break
                g.key("ret")
        if WINDOWS:
            # READY comes from the batch file Windows runs: without it the
            # prompt would be plain DOS, after a Windows that did not start.
            g.wait(r"READY[\s\S]*C:\\W>$", 240)
        else:
            g.wait(r"C:\\W>$", 90)
        check_batch(g)
        check_confinement(g)
        check_bad_allow(g)
        check_edit(g)
        if not os.environ.get("GORK_DOS_ONE_DISK"):
            check_allow(g)
        check_long(g)
        check_run(g)
        if RELAY:
            check_spx(g)
        check_timeout(g)
        check_tui(g)
    finally:
        g.close()
        srv.shutdown()
        if relay:
            relay.terminate()
    # CR LF must have survived the edit: read the file back off the disk
    out = subprocess.run(["mtype", "-i", "%s@@%d" % (disk, (63 if SMALL else 2048) * 512),
                          "::W/TWO.TXT"], capture_output=True, check=True)
    assert out.stdout == b"one\r\n2\r\nthree\r\n", out.stdout
    print("all DOS checks passed")


if __name__ == "__main__":
    main()
