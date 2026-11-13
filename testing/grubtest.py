#!/usr/bin/env python3
"""Run the `gork` command of grubbuild.sh's GRUB, in QEMU.

Usage:  python3 testing/grubtest.py PACKAGE FAKEAPI

PACKAGE is gork-<version>-grub-x64.zip, -grub-aa64.zip or -grub-i386.zip,
FAKEAPI testing/fakeapi.c built for this machine.  For the first two, GRUB
(EFI/BOOT/BOOTX64.EFI or BOOTAA64.EFI, with gork in it) boots from a FAT
disk under OVMF in a q35 machine, or AAVMF in a virt one, with a virtio
network card on QEMU's user network, which takes it to the fake API on
10.0.2.2.  The i386 package's gork.pxe is fetched by a card's PXE ROM
instead -- the only way a BIOS PC has a card at all -- from QEMU's own
TFTP server, and its grub.cfg comes the same way: QEMU serves DIRECTORY as
the root, so the checks' grub.cfg and gork.cnf are written in a temporary
one.  The firmware runs
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


# The checks, for the BIOS package: GRUB's console is the serial line, and
# the commands come from this file rather than from keys typed at the
# prompt, which race it.  Same checks as the typed ones below.
PXE_CFG = """serial --unit=0 --speed=115200
terminal_output serial console
echo ==prefix $prefix
echo ==version
gork --version
echo ==notui
gork
echo ==hello
gork hello
echo ==seed
set GORK_DIR=(gork)/w
gork w1
echo ==go
set GORK_ALLOW_RUN=1
gork go
unset GORK_ALLOW_RUN
echo ==esc
gork esc
echo ==sub
set GORK_DIR=(gork)/w/sub
gork w1
echo ==ro
set GORK_DIR=$prefix
gork w1
echo ==slow
set GORK_DIR=(gork)/w
set AGENT_TIMEOUT=5
gork slow; echo ==status $?
unset AGENT_TIMEOUT
sleep 31
echo ==again
gork hello
echo ==out
cat (gork)/w/out.txt
echo ==w1
cat (gork)/w/sub/w1.txt
echo ==ls
ls (gork)/
echo ==END
"""


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
        boots = [n for n in z.namelist() if n.startswith("EFI/BOOT/BOOT")]
        pxe = None if boots else z.extract("gork.pxe", esp)
    if boots:
        with zipfile.ZipFile(pkg) as z:
            z.extract(boots[0], esp)
        boot, fw, machine = boots[0], *MACHINES[os.path.basename(boots[0])]
    else:
        # A BIOS PC: no firmware of its own to speak of, and no disk to
        # read -- the PXE ROM fetches gork.pxe, and GRUB's prefix is on
        # the TFTP server, which is this directory.
        boot = None
        tftp = os.path.join(t, "tftp")
        os.makedirs(os.path.join(tftp, "boot", "grub"))
        shutil.copy(pxe, os.path.join(tftp, "gork.pxe"))
        fw = None
        machine = ["qemu-system-i386", "-m", "256", "-boot", "n",
                   "-netdev", "user,id=n0,tftp=" + tftp + ",bootfile=gork.pxe",
                   "-device", "rtl8139,netdev=n0"]
    # the settings beside grub.cfg, in $prefix; and a file to read
    prefix = os.path.join(tftp, "boot", "grub") if boot is None else \
             os.path.join(esp, "EFI", "BOOT")
    with open(os.path.join(prefix, "gork.cnf"), "w") as f:
        f.write("relay = 10.0.2.2:%d\napi_key = sk-fake\ntimeout = 20\n"
                "retries = 0\n" % PORT)
    extra = []
    if boot is None:
        # GRUB's own console is the serial line here, not a firmware's, so
        # grub.cfg turns it on first; the file it runs is fetched by GRUB
        # from the same TFTP server as gork.pxe.
        with open(os.path.join(prefix, "grub.cfg"), "w") as f:
            f.write(PXE_CFG)
    else:
        img = os.path.join(t, "esp.img")
        sh("mkfs.fat", "-C", "-F", "16", "-n", "GORK", img, "65536",
           stdout=subprocess.DEVNULL)
        sh("mcopy", "-i", img, "-s", "-Q", os.path.join(esp, "EFI"), "::/")
        vars_fd = os.path.join(t, "vars.fd")
        shutil.copy(fw % "VARS", vars_fd)
        extra = ["-drive", "if=pflash,format=raw,readonly=on,file=" + fw % "CODE",
                 "-drive", "if=pflash,format=raw,file=" + vars_fd,
                 "-drive", "if=none,id=esp,format=raw,file=" + img] + NOPXE

    fp = subprocess.Popen([fake, str(PORT)], cwd=fdir, stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL)
    c = Console(machine + [
                 "-display", "none", "-serial", "stdio", "-monitor", "none"]
                + extra)
    part = {}
    if boot is None:
        # the whole run comes from PXE_CFG: wait for the end, not a prompt
        try:
            c.expect(b"==END", 600)
        except TimeoutError as e:
            print("grubtest: " + str(e))
        finally:
            c.p.kill()
            fp.terminate()
            fp.wait()
        return report(c, fdir, part, fake, t)
    try:
        c.expect(PROMPT, 180)
        # how GRUB's clock keeps up with real time: in the log, for when
        # a timeout check goes wrong
        t0 = time.time()
        c.cmd("sleep 5")
        print("grubtest: GRUB's sleep 5 took %.1f s" % (time.time() - t0))
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
        # The fake takes 30 s.  A timeout of 5, not the file's 20: GRUB's
        # clock, from a TSC calibrated at boot, can run slow in an
        # emulator on a busy host, and twice let 30 s pass as less than 20
        # (Jenkins #17, #18).  Then wait for the fake, which serves one
        # request at a time, in real time.
        c.cmd("set AGENT_TIMEOUT=5")
        part["slow"] = c.cmd("gork slow; echo ==status $?", 240)
        c.cmd("unset AGENT_TIMEOUT")
        time.sleep(30)
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
    return report(c, fdir, part, fake, t)


def sections(text):
    """The serial transcript cut at its ==MARKER lines, into {marker: text}:
    each marker in PXE_CFG names the check its command feeds.  A command
    that echoes its own status puts it at the top of that section."""
    got, name, cur = {}, "boot", []
    for line in text.splitlines():
        line = re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", line)
        if line.startswith("=="):
            # the marker is the previous command's output -- `echo ==status
            # $?` is how a command reports its status -- so it stays there,
            # and the new section starts with the blank line the echoed
            # command line leaves, as the typed checks see it.
            cur.append(line)
            got[name] = "\n" + "\n".join(cur)
            name, cur = line[2:].strip(), []
        else:
            cur.append(line)
    got[name] = "\n" + "\n".join(cur)
    return got


def report(c, fdir, part, fake, t):
    out = c.all.decode("utf-8", "replace").replace("\r", "")
    if os.environ.get("GRUBTEST_VERBOSE"):
        print(out)
    if not part and b"==END" in c.all:
        # The PXE run is one transcript: each ==MARKER in PXE_CFG names the
        # check it feeds, and the command's own echo lands in it.
        part = sections(out)

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
