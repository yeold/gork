#!/usr/bin/env python3
"""Run the `gork` command of grubbuild.sh's GRUB, in QEMU.

Usage:  python3 testing/grubtest.py PACKAGE FAKEAPI

PACKAGE is gork-<version>-grub-x64.zip or -grub-aa64.zip, FAKEAPI
testing/fakeapi.c built for this machine.  GRUB (EFI/BOOT/BOOTX64.EFI or
BOOTAA64.EFI, with gork in it) boots from a FAT disk under OVMF in a q35
machine, or AAVMF in a virt one, with a virtio network card on QEMU's user
network, which takes it to the fake API on 10.0.2.2.  The firmware runs
without its own network stack (no random number generator: no virtio one,
and an arm64 processor from before RNDR), so the card is GRUB's and
gork's alone.  The checks are typed at GRUB's prompt
on the serial console; the files they leave are in (gork), in RAM, and
looked at with GRUB's own `cat`.  Prints "ok:" or "FAIL:" per check and
exits 1 if any failed.  Needs qemu-system-x86_64 and OVMF, or
qemu-system-aarch64 and qemu-efi-aarch64, mtools and dosfstools.
"""
import json, os, re, select, shutil, subprocess, sys, tempfile, time, zipfile

PORT = 8099
PROMPT = b"grub> "
NOPXE = ["-fw_cfg", "name=opt/org.tianocore/IPv4PXESupport,string=n",
         "-fw_cfg", "name=opt/org.tianocore/IPv6PXESupport,string=n"]
# the boot file, the firmware's code and variables, and the machine; on
# arm64 the card without iPXE's ROM (x86's), as AAVMF has a driver for it
MACHINES = {
    "BOOTX64.EFI": ("/usr/share/OVMF/OVMF_%s_4M.fd",
                    ["qemu-system-x86_64", "-machine", "q35", "-m", "512",
                     "-device", "ide-hd,drive=esp,bus=ide.0,bootindex=0",
                     "-nic", "user,model=virtio-net-pci"]),
    "BOOTAA64.EFI": ("/usr/share/AAVMF/AAVMF_%s.fd",
                     ["qemu-system-aarch64", "-M", "virt", "-cpu", "cortex-a72",
                      "-m", "1024",
                      "-device", "virtio-blk-pci,drive=esp,bootindex=0",
                      "-netdev", "user,id=n",
                      "-device", "virtio-net-pci,netdev=n,romfile="]),
}


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
            time.sleep(0.02)

    def cmd(self, line, secs=120):
        """A command line at the prompt: what it printed."""
        self.send(line + "\r")
        return self.expect(PROMPT, secs).replace("\r", "")


def sh(*a, **kw):
    subprocess.run(a, check=True, **kw)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    pkg, fake = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
    t = tempfile.mkdtemp(prefix="grubtest.")
    esp, fdir = os.path.join(t, "esp"), os.path.join(t, "f")
    os.makedirs(fdir)
    with zipfile.ZipFile(pkg) as z:
        boot = [n for n in z.namelist() if n.startswith("EFI/BOOT/BOOT")][0]
        z.extract(boot, esp)
    fw, machine = MACHINES[os.path.basename(boot)]
    # the settings beside grub.cfg, in $prefix; and a file to read
    with open(os.path.join(esp, "EFI", "BOOT", "gork.cnf"), "w") as f:
        f.write("relay = 10.0.2.2:%d\napi_key = sk-fake\ntimeout = 20\n"
                "retries = 0\n" % PORT)
    img = os.path.join(t, "esp.img")
    sh("mkfs.fat", "-C", "-F", "16", "-n", "GORK", img, "65536",
       stdout=subprocess.DEVNULL)
    sh("mcopy", "-i", img, "-s", "-Q", os.path.join(esp, "EFI"), "::/")
    vars_fd = os.path.join(t, "vars.fd")
    shutil.copy(fw % "VARS", vars_fd)

    fp = subprocess.Popen([fake, str(PORT)], cwd=fdir, stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL)
    c = Console(machine + [
                 "-display", "none", "-serial", "stdio", "-monitor", "none",
                 "-drive", "if=pflash,format=raw,readonly=on,file=" + fw % "CODE",
                 "-drive", "if=pflash,format=raw,file=" + vars_fd,
                 "-drive", "if=none,id=esp,format=raw,file=" + img]
                + NOPXE)
    part = {}
    try:
        c.expect(PROMPT, 180)
        part["prefix"] = c.cmd("echo ==prefix $prefix")
        part["version"] = c.cmd("gork --version")
        part["notui"] = c.cmd("gork")
        part["hello"] = c.cmd("gork hello", 240)
        c.cmd("set GORK_DIR=(gork)/w")
        part["seed"] = c.cmd("gork w1", 240)      # a file for list_dir
        c.cmd("set GORK_ALLOW_RUN=1")
        part["go"] = c.cmd("gork go", 300)
        c.cmd("unset GORK_ALLOW_RUN")
        part["esc"] = c.cmd("gork esc", 240)
        c.cmd("set GORK_DIR=(gork)/w/sub")
        part["sub"] = c.cmd("gork w1", 240)
        c.cmd("set GORK_DIR=$prefix")
        part["ro"] = c.cmd("gork w1", 240)
        c.cmd("set GORK_DIR=(gork)/w")
        part["slow"] = c.cmd("gork slow; echo ==status $?", 240)
        part["again"] = c.cmd("gork hello", 240)
        part["out"] = c.cmd("cat (gork)/w/out.txt")
        part["w1"] = c.cmd("cat (gork)/w/sub/w1.txt")
        part["ls"] = c.cmd("ls (gork)/")
    except TimeoutError as e:
        print("grubtest: " + str(e))
    finally:
        c.p.kill()
        fp.terminate()
        fp.wait()
    out = c.all.decode("utf-8", "replace").replace("\r", "")
    if os.environ.get("GRUBTEST_VERBOSE"):
        print(out)

    log = ""
    if os.path.exists(os.path.join(fdir, "fakeapi.log")):
        with open(os.path.join(fdir, "fakeapi.log")) as f:
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
        ("write_file, in (gork), read by GRUB's cat", "a\nb" in p("out")),
        ("read_file", "a\nb\n" in results),
        ("list_dir", any("w1.txt" in r and "out.txt" not in r for r in results)),
        ("run_command, its output taken from GRUB's console",
         any("uname" in r and "[exit " in r for r in results)),
        ("paths outside the working directory refused",
         "outside the working directory" in log and "escape.txt" not in p("ls")),
        ("a write in a subdirectory", "\nx\n" in p("w1")),
        ("disks are read-only", "read-only file system" in log),
        ("the request timeout", "timeout on socket" in p("slow")),
        ("exit status in $?", "==status 1" in p("slow")),
        ("a run after a failed one starts afresh", "PONG" in p("again")),
    ]
    fails = 0
    for name, good in checks:
        print(("ok: " if good else "FAIL: ") + name)
        fails += not good
    if fails and not os.environ.get("GRUBTEST_VERBOSE"):
        print(out[-3000:])
    shutil.rmtree(t, ignore_errors=True)
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
