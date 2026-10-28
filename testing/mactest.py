#!/usr/bin/env python3
"""Run gork for the 68k Mac (macbuild.sh) on System 7.5.3, in Basilisk II.

Usage:  python3 testing/mactest.py PACKAGE FAKEAPI ROM

PACKAGE is gork-<version>-mac68k.zip, FAKEAPI testing/fakeapi.c built for
this machine, ROM a Quadra 800's (or a Quadra 650's, a Centris 650's: the
1 MB ROM F1ACAD13), which is Apple's and not to be had here.  System 7.5.3
comes from the Internet Archive, checksums checked, into $GORK_VM/mactest
(~/.cache/gork-vm/mactest): a volume with it installed, and Apple's CD for
MacTCP, which is set up as its control panel would be (Ethernet, the
address from the network, slirp's name server).  gork starts from the
System Folder's Startup Items with a gork.in beside it, its command lines;
it writes what it shows to gork.out and turns the Mac off at the end.  Its
work is in the volume's folder w, read with machfs when Basilisk II is
done.  The emulated Ethernet is slirp's, as QEMU's: the fake API on
10.0.2.2.  Prints "ok:" or "FAIL:" per check and exits 1 if any failed.

Needs BasiliskII (Debian's basilisk2, in contrib), Xvfb when there is
no $DISPLAY, Python's machfs and macresources (pip), and as root setpriv, as Basilisk II runs as nobody
(as root it asks for real-time threads, which a container refuses).

Why not QEMU's Quadra 800: it starts System 7 from a CD, but its SCSI
disks need a driver on them, which Apple's tools do not put on a disk they
do not know; Basilisk II starts from an HFS volume as it is.
"""
import hashlib, io, json, os, shutil, struct, subprocess, sys, tempfile
import time, urllib.request, zipfile

PORT = 8099
VM = os.path.join(os.environ.get("GORK_VM",
                                 os.path.expanduser("~/.cache/gork-vm")),
                  "mactest")
IA = "https://archive.org/download/"
SYSTEM = (IA + "AppleMacintoshSystem753/System7_5_3.img",
          "7a70976e590d464c3476c0b40500ae589f77604a26b1c8bd310cd4922233ec87")
CD = (IA + "Macintosh-68K-PPC-System-7.5.3-Bootable-ISO/System753%20691-1079-A.zip",
      "sha1:b79a3398ba3e1e5a37e769a540d95825511d9e21")
VOLUME = "Macintosh HD"
WORK = "/" + VOLUME + "/w"

# MacTCP's settings, as its control panel leaves them in its resources:
# in 'ipln' 128 from byte 91, the link (Ethernet, Basilisk II's) and the
# address from the network (BootP, slirp's); and 'dnsl' 128, slirp's name
# server, 10.0.2.3, for every domain
IPLN_LINK = bytes.fromhex(
    "81 00 00 00 00 00 14 00 00 00 02 00 04 20 14 00 09 3e 82 44 52 56 52"
    "00 00 00 0b 00 00 00 8f 03 fd 00 02 44 52 00")
DNSL = bytes.fromhex("00 01 0a 00 02 03 01 2e 00 52")

# gork's command lines, one at each prompt; set NAME=value as the prompt has it
SCRIPT = [
    ("version", "--version"),
    ("notui", "gork"),
    ("hello", "hello"),
    (None, 'set "GORK_DIR=%s"' % WORK),
    ("seed", "w1"),                     # a file for list_dir
    ("roman", "w2"),                    # and a name beyond ASCII
    ("go", "go"),
    ("over", "w1"),                     # over the file w1 left
    ("esc", "esc"),
    (None, 'set "GORK_DIR=%s/sub"' % WORK),
    ("sub", "w1"),
    (None, 'set "GORK_DIR=%s"' % WORK),
    ("slow", "slow"),
    ("again", "hello"),
    (None, "shutdown"),
]


def fetch(url, digest):
    """A download, once, into VM, its checksum checked."""
    os.makedirs(VM, exist_ok=True)
    path = os.path.join(VM, os.path.basename(url).replace("%20", " "))
    if not os.path.exists(path):
        print("fetching", url)
        urllib.request.urlretrieve(url, path + ".part")
        os.replace(path + ".part", path)
    algo, _, want = digest.rpartition(":")
    h = hashlib.new(algo or "sha256")
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    assert h.hexdigest() == want, path
    return path


def find(folder, want):
    import machfs
    for name, x in folder.items():
        if isinstance(x, machfs.Folder):
            r = find(x, want)
            if r is not None:
                return r
        elif name == want:
            return x


def mactcp():
    """MacTCP from the CD, set up; kept in VM as MacBinary-less parts."""
    import machfs, macresources
    keep = os.path.join(VM, "MacTCP.rsrc")
    if not os.path.exists(keep):
        with zipfile.ZipFile(fetch(*CD)) as z:
            iso = z.read(z.namelist()[0])
        v = machfs.Volume()
        v.read(iso[192 * 512:])         # the HFS partition, after the map
        with open(keep + ".part", "wb") as f:
            f.write(find(v, "MacTCP").rsrc)
        os.replace(keep + ".part", keep)
    with open(keep, "rb") as f:
        rs = list(macresources.parse_file(f.read()))
    for r in rs:
        if (r.type, r.id) == (b"ipln", 128):
            d = bytearray(r.data)
            d[91:91 + len(IPLN_LINK)] = IPLN_LINK
            r.data = bytes(d)
        elif (r.type, r.id) == (b"dnsl", 128):
            r.data = DNSL
    f = machfs.File()
    f.type, f.creator = b"cdev", b"ztcp"
    f.rsrc = macresources.make_file(rs)
    return f


def macbinary(b):
    import machfs
    dlen, rlen = struct.unpack(">II", b[83:91])
    r0 = 128 + (dlen + 127) // 128 * 128
    f = machfs.File()
    f.type, f.creator = b[65:69], b[69:73]
    f.data, f.rsrc = b[128:128 + dlen], b[r0:r0 + rlen]
    return f


def text(s):
    """A TEXT file as SimpleText keeps one: Mac Roman, CR; gork's creator,
    so the Finder hands it to gork at startup rather than ask what opens it"""
    import machfs
    f = machfs.File()
    f.type, f.creator = b"TEXT", b"GoRk"
    f.data = s.replace("\n", "\r").encode("mac_roman")
    return f


def disk(pkg, path):
    import machfs
    v = machfs.Volume()
    with open(fetch(*SYSTEM), "rb") as f:
        v.read(f.read())
    v.name = VOLUME
    sf = v["System Folder"]
    sf["Control Panels"]["MacTCP"] = mactcp()
    si = sf["Startup Items"] = machfs.Folder()
    with zipfile.ZipFile(pkg) as z:
        si["gork"] = macbinary(z.read("gork.bin"))
    si["gork.in"] = text("".join(line + "\n" for _, line in SCRIPT))
    si["gork.cnf"] = text("relay = 10.0.2.2:%d\napi_key = sk-fake\n"
                          "timeout = 20\nretries = 0\n" % PORT)
    w = v["w"] = machfs.Folder()
    w["sub"] = machfs.Folder()
    with open(path, "wb") as f:
        f.write(v.write(size=48 * 1024 * 1024))


def basilisk(t, img, rom, secs):
    """Basilisk II until gork turns the Mac off: whether it did in time."""
    home = os.path.join(t, "home")
    os.makedirs(home)
    with open(os.path.join(home, ".basilisk_ii_prefs"), "w") as f:
        f.write("rom %s\ndisk %s\nextfs %s\nramsize 67108864\nmodelid 14\n"
                "cpu 4\nfpu true\nether slirp\nscreen win/640/480\n"
                "nosound true\nnocdrom true\nnogui true\n"
                % (rom, img, os.path.join(t, "share")))
    os.makedirs(os.path.join(t, "share"))
    env = dict(os.environ, HOME=home, SDL_AUDIODRIVER="dummy")
    xvfb = None
    if not env.get("DISPLAY"):          # GTK wants one, nogui or not
        n = 97
        while os.path.exists("/tmp/.X11-unix/X%d" % n):
            n += 1
        xvfb = subprocess.Popen(["Xvfb", ":%d" % n, "-screen", "0", "800x600x24"],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(100):
            if os.path.exists("/tmp/.X11-unix/X%d" % n):
                break
            time.sleep(0.1)
        env["DISPLAY"] = ":%d" % n
    cmd = ["BasiliskII"]
    if os.geteuid() == 0:
        for root, dirs, files in os.walk(t):
            for n in dirs + files:
                os.chmod(os.path.join(root, n), 0o777)
        os.chmod(t, 0o777)
        cmd = ["setpriv", "--reuid=65534", "--regid=65534", "--clear-groups"] + cmd
    with open(os.path.join(t, "basilisk.log"), "w") as log:
        p = subprocess.Popen(cmd, env=env, stdout=log, stderr=log)
        try:
            p.wait(secs)
            return True
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
            return False
        finally:
            if xvfb:
                xvfb.terminate()
                xvfb.wait()


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    import machfs
    pkg, fake, rom = (os.path.abspath(a) for a in sys.argv[1:])
    t = tempfile.mkdtemp(prefix="mactest.")
    fdir = os.path.join(t, "f")
    os.makedirs(fdir)
    img = os.path.join(t, "hd.img")
    disk(pkg, img)

    fp = subprocess.Popen([fake, str(PORT)], cwd=fdir, stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL)
    try:
        done = basilisk(t, img, rom, 900)
    finally:
        fp.terminate()
        fp.wait()
    if not done:
        print("mactest: the Mac was still on after 900 s")

    v = machfs.Volume()
    with open(img, "rb") as f:
        v.read(f.read())
    si = v["System Folder"]["Startup Items"]
    out = si["gork.out"].data.decode("mac_roman").replace("\r", "\n") \
        if "gork.out" in si else ""
    if os.environ.get("MACTEST_VERBOSE"):
        print(out)
    # gork.out a part to each command line: what follows its prompt
    part, rest = {}, out.split("\ngork> ")[1:]
    for (key, _), chunk in zip(SCRIPT, rest):
        if key:
            part[key] = chunk
    p = lambda k: part.get(k, "")

    log = ""
    if os.path.exists(os.path.join(fdir, "fakeapi.log")):
        with open(os.path.join(fdir, "fakeapi.log"), encoding="utf-8",
                  errors="replace") as f:
            log = f.read()
    results, offered = [], set()        # each tool_result's text; the tools
    for line in log.splitlines():
        try:
            body = json.loads(line)
            offered.update(x.get("name") for x in body.get("tools", []))
            for m in body.get("messages", [])[-1:]:
                for b in m["content"] if isinstance(m["content"], list) else []:
                    if b.get("type") == "tool_result":
                        r = b.get("content")
                        results.append(r if isinstance(r, str) else json.dumps(r))
        except (ValueError, KeyError, TypeError, AttributeError):
            pass

    w = v["w"]
    get = lambda d, n: d[n] if n in d else None
    outf, roman = get(w, "out.txt"), get(w, "räksmörgås.txt")
    sub = get(w["sub"], "w1.txt") if "sub" in w else None
    checks = [
        ("--version", "gork " in p("version")),
        ("batch mode only", "no TUI" in p("notui")),
        ("a reply, over MacTCP", "PONG" in p("hello")),
        ("write_file: a SimpleText file, its lines ending in CR",
         outf is not None and outf.data == b"a\rb\r"
         and (outf.type, outf.creator) == (b"TEXT", b"ttxt")),
        ("read_file: LF again", "a\nb\n" in results),
        ("list_dir", any("w1.txt" in r and "out.txt" not in r for r in results)),
        ("a name in Mac Roman on the Mac, in UTF-8 to gork",
         roman is not None and any("räksmörgås.txt" in r for r in results)),
        ("text in Mac Roman, ? for what it lacks",
         roman is not None and roman.data == "€ 5 — ?\r".encode("mac_roman")),
        ("no run_command on the Mac", offered and "run_command" not in offered),
        ("paths outside the working directory refused",
         "outside the working directory" in log and "escape.txt" not in v),
        ("a write in a subdirectory", sub is not None and sub.data == b"x"),
        ("a write over a file, no temporary file left",
         "write_file w1.txt" in p("over") and get(w, "w1.txt") is not None
         and not any("write failed" in r or "cannot create" in r for r in results)
         and not any(n.endswith(".g$$") for n in w)),
        ("the request timeout", "timeout on socket" in p("slow")),
        ("exit status", "==status 1" in p("slow")
         and "==status 0" in p("hello")),
        ("a run after a failed one starts afresh", "PONG" in p("again")),
        ("shut down at the end of gork.in", done),
    ]
    fails = 0
    for name, good in checks:
        print(("ok: " if good else "FAIL: ") + name)
        fails += not good
    if fails and not os.environ.get("MACTEST_VERBOSE"):
        print(out[-3000:])
        with open(os.path.join(t, "basilisk.log"), errors="replace") as f:
            print(f.read()[-1500:])
    if os.environ.get("MACTEST_KEEP"):
        print("mactest: kept", t)
    else:
        shutil.rmtree(t, ignore_errors=True)
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
