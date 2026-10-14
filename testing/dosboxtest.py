#!/usr/bin/env python3
"""Run the 16-bit gork.exe in DOSBox-X as an 8086 and as a 286 and check it.

Usage:  python3 testing/dosboxtest.py [gork-<version>-dos16.zip]

QEMU has nothing below a 486, which runs 386 instructions without a word;
DOSBox-X's 8086 and 286 do not.  On the 286 gork talks to a fake API
through DOSBox-X's NE2000 and the Crynwr packet driver: a tool call that
writes a file and one that runs a command.  On the 8086 that network does
not work (the driver reads the card a byte at a time there, and the
emulated NE2000 loses those reads), so the 8086 runs what needs no
network: --version, and the TUI, typed into by AUTOTYPE.  Needs dosbox-x
and unzip; the fake API and the packet driver are dostest.py's.
"""
import glob, os, subprocess, sys, tempfile, threading, http.server, zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dostest as T


def dosbox(cpu, c, cmds, net=False, secs=240):
    """Run `cmds` in DOSBox-X as `cpu`, with directory `c` as C:."""
    args = ["dosbox-x", "-silent", "-nogui", "-nomenu",
            "-set", "cpu cputype=%s" % cpu, "-set", "cpu core=normal",
            "-set", "cpu cycles=max"]
    if net:
        args += ["-set", "ne2000 ne2000=true", "-set", "ne2000 nicbase=300",
                 "-set", "ne2000 nicirq=3", "-set", "ne2000 backend=slirp"]
    args += ["-c", "mount c %s" % c, "-c", "c:"]
    for cmd in cmds + ["exit"]:
        args += ["-c", cmd]
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    subprocess.run(args, env=env, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL, timeout=secs)


def read(c, name):
    for n in os.listdir(c):
        if n.upper() == name.upper():
            with open(os.path.join(c, n), errors="replace") as f:
                return f.read()
    return None


def drive(zip_path):
    """A fresh C: with gork.exe, the packet driver and a work directory."""
    c = tempfile.mkdtemp(prefix="dosbox")
    with zipfile.ZipFile(zip_path) as z:
        with open(os.path.join(c, "GORK.EXE"), "wb") as f:
            f.write(z.read("gork.exe"))
    with zipfile.ZipFile(os.path.join(T.VM, "crynwr.zip")) as z:
        with open(os.path.join(c, "NE2000.COM"), "wb") as f:
            f.write(z.read("DRIVERS/CRYNWR/NE2000.COM"))
    os.makedirs(os.path.join(c, "W"))
    for name, body in {
            "WATTCP.CFG": "my_ip = 10.0.2.15\nnetmask = 255.255.255.0\n"
                          "gateway = 10.0.2.2\nnameserver = 10.0.2.3\n",
            "GORK.CNF": "relay = 10.0.2.2:%d\nhost = fake.local\n"
                        "timeout = 20\nretries = 0\n" % T.PORT,
            "W/IN.TXT": "inside\n"}.items():
        with open(os.path.join(c, name), "w", newline="\r\n") as f:
            f.write(body)
    return c


def check_286(zip_path):
    """Two round trips through the network: write_file, run_command."""
    c = drive(zip_path)
    del T.requests[:]
    T.script[:] = [T.tool("write_file", path="out.txt", content="hello\n"),
                   T.text("wrote it"),
                   T.tool("run_command", command="type in.txt"),
                   T.text("ran it")]
    dosbox("286", c, ["set WATTCP.CFG=C:\\", "set GORK_ALLOW_RUN=1",
                      "NE2000 0x60 3 0x300 > NUL", "cd W",
                      "..\\gork t > ..\\B1.TXT", "..\\gork t > ..\\B2.TXT"],
           net=True)
    b1, b2 = read(c, "B1.TXT"), read(c, "B2.TXT")
    assert b1 is not None and "wrote it" in b1, b1
    assert (read(os.path.join(c, "W"), "OUT.TXT") or "").strip() == "hello"
    assert b2 is not None and "ran it" in b2, b2
    assert "inside" in T.results()[-1]["content"], T.results()
    print("ok: 286: write_file and run_command over the network")


def check_8086(zip_path):
    """--version, and the TUI: the trust prompt, the line editor, a task
    that fails for want of a network, and /quit."""
    c = drive(zip_path)
    dosbox("8086", c, [
        "gork --version > V.TXT", "cd W",
        "autotype -w 4 -p 1 y enter i x home h end bspace enter"
        " slash q u i t enter",
        "..\\gork", "echo done > ..\\DONE.TXT"])
    v = read(c, "V.TXT")
    assert v is not None and v.startswith("gork "), v
    assert read(c, "DONE.TXT") is not None, "the TUI did not exit"
    assert read(os.path.join(c, "_GORK"), "TRUSTED") is not None, \
        "the trust prompt was not answered"
    # /quit deletes the TUI's capture file.  It is left behind when gork
    # ends some other way: as Watt-32 once ended it, finding no packet
    # driver for the task.
    left = [n.upper() for n in os.listdir(os.path.join(c, "W"))]
    assert left == ["IN.TXT"], left
    print("ok: 8086: --version, and the TUI")


def main():
    zips = sorted(glob.glob("out/gork-*-dos16.zip"), key=os.path.getmtime)
    zip_path = sys.argv[1] if len(sys.argv) > 1 else (zips[-1] if zips else None)
    if zip_path is None:
        sys.exit("no gork-*-dos16.zip; run `sh dos16build.sh` first")
    T.DOWNLOADS[:] = [d for d in T.DOWNLOADS if "crynwr" in d[0]]
    T.fetch()
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", T.PORT), T.Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    check_286(os.path.abspath(zip_path))
    check_8086(os.path.abspath(zip_path))
    print("all DOSBox-X checks passed")


if __name__ == "__main__":
    main()
