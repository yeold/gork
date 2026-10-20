#!/usr/bin/env python3
"""Run the BSD packages from bsdbuild.sh on their own systems, in QEMU.

Usage:  python3 testing/bsdtest.py TARGET PACKAGE FAKEAPI [IMAGE]

TARGET is a bsdbuild.sh target (freebsd-amd64, ...), PACKAGE its
gork-<version>-TARGET.tar.gz, FAKEAPI the fake API built beside it.
IMAGE is the system's own VM image; without it, it is fetched into
$GORK_VM (default /tmp/gork-vm) and its checksum checked.  The image is
booted with -snapshot, so it is never changed.  gork, the fake API,
testing/vmcheck.sh and testing/mcp.sh go in on a CD; the checks run as root
on the serial console.  Needs qemu-system-x86_64 with OVMF (and -aarch64 with
its UEFI firmware for arm64), xorriso or genisoimage, curl, xz and gzip.
"""
import hashlib, os, shutil, subprocess, sys, tarfile, tempfile, time, select

HERE = os.path.dirname(os.path.abspath(__file__))
FB = "https://download.freebsd.org/releases/VM-IMAGES/15.1-RELEASE"
NB = "https://cdn.netbsd.org/pub/NetBSD"
OB = "https://cdn.openbsd.org/pub/OpenBSD/7.9"

# image: (url, sha256 of the download, how it is packed, disk format)
IMAGES = {
    "freebsd-amd64": (FB + "/amd64/Latest/FreeBSD-15.1-RELEASE-amd64-ufs.qcow2.xz",
                      "25ad16e9d0c037f40a43c6f3c474b5c1ba62eaf9204dbd7f034fdbffe2c3dae5", "xz", "qcow2"),
    "freebsd-arm64": (FB + "/aarch64/Latest/FreeBSD-15.1-RELEASE-arm64-aarch64-ufs.qcow2.xz",
                      "b61e4f47886332aaff9eb88cfd40c4202132a5c7bf8b56d5b4221f29b683eac2", "xz", "qcow2"),
    "netbsd-amd64": (NB + "/images/11.0/NetBSD-11.0-amd64-live.img.gz",
                     "70df2336baea29348243b793f25481ea6e8445e1a8b783d657488e2f391b887a", "gzip", "raw"),
    # NetBSD lists only the unpacked image's SHA512 (ce7f9521...); this is
    # the download's, taken after that matched
    "netbsd-arm64": (NB + "/NetBSD-11.0/evbarm-aarch64/binary/gzimg/arm64mbr.img.gz",
                     "5b6ed67e84548d8eb270da5f89c772a622b8803981afc06b6cfa880a80fdb02c", "gzip", "raw"),
    # OpenBSD has no ready image: its installer, which openbsd_disk() runs
    # once, unattended, into a disk kept beside it
    "openbsd-amd64": (OB + "/amd64/install79.img",
                      "210b4a4ef8966960b0dc2a08eb3c7d1fc20088902968d3e9b5fe7d17a18634f5", None, "qcow2"),
    "openbsd-arm64": (OB + "/arm64/install79.img",
                      "551e411e46739a244376592568b3e131f029aebdce63030a7a42901dac292b8c", None, "qcow2"),
}
PASSWORD = "gork123"            # root's, on the OpenBSD the test installs
# the answers to OpenBSD's installer (autoinstall(8)); the sets come from
# the install image itself, which was checked against OpenBSD's SHA256.
# A key must be a whole question, and the questions differ a little
# between machines: amd64 offers no "(O)penBSD area", hence both forms of
# the partitioning one (unanswered, it is an MBR disk UEFI does not boot)
INSTALL_CONF = """System hostname = gorktest
Password for root account = %s
Allow root ssh login = no
What timezone are you in = UTC
Do you expect to run the X Window System = no
Change the default console to com0 = yes
Which speed should com0 use = 115200
Setup a user = no
Which disk is the root disk = sd0
Encrypt the root disk with a (p)assphrase or (k)eydisk = no
Use (W)hole disk MBR, whole disk (G)PT or (E)dit = gpt
Use (W)hole disk MBR, whole disk (G)PT, (O)penBSD area or (E)dit = gpt
An EFI/GPT disk may not boot. Proceed = yes
Use (A)uto layout, (E)dit auto layout, or create (C)ustom layout = a
Location of sets = disk
Is the disk partition already mounted = no
Which disk contains the install media = sd1
Which sd1 partition has the install sets = a
Pathname to the sets = 7.9/%s
Set name(s) = -x* -game* -man* -comp*
Directory does not contain SHA256.sig. Continue without verification = yes
Continue without verification = yes
"""
# how each system mounts the CD and powers off
MOUNT = {
    "freebsd": "mkdir -p /cd && mount -t cd9660 /dev/cd0 /cd",
    "netbsd": "mkdir -p /cd && mount -t cd9660 /dev/cd0a /cd",
    # OpenBSD's CD is a second virtio disk: a SCSI or SATA one would change
    # which disk the installed system boots and roots from
    "openbsd": "mkdir -p /cd && mount -t cd9660 /dev/sd1c /cd",
}
OFF = {"freebsd": "poweroff", "netbsd": "poweroff", "openbsd": "halt -p"}
# answers to the boot loader, before the kernel starts: NetBSD's EFI loader
# leaves the kernel's console on the (absent) screen unless told
BOOT = {
    "netbsd-amd64": [(b"Choose an option", "3"), (b"> ", "consdev com0\r"),
                     (b"> ", "boot\r")],
}


def fetch(target):
    url, sha, pack, _ = IMAGES[target]
    d = os.environ.get("GORK_VM", "/tmp/gork-vm")
    os.makedirs(d, exist_ok=True)
    raw = os.path.join(d, target + "-" + os.path.basename(url))
    img = raw.rsplit(".", 1)[0] if pack else raw
    ok = img + ".ok"                # written once the image is whole
    if os.path.exists(ok) and os.path.exists(img):
        return img
    # into a .part first: an interrupted download must not pass for one
    part = raw + ".part"
    subprocess.run(["curl", "-fsSL", "-o", part, url], check=True)
    h = hashlib.sha256()
    with open(part, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    if h.hexdigest() != sha:
        os.remove(part)
        sys.exit("bsdtest: checksum mismatch for " + url)
    os.rename(part, raw)
    if pack:                        # unpacked in place: the images are big
        subprocess.run([pack, "-df", raw], check=True)
    open(ok, "w").close()
    return img


def machine(arch, w, disks):
    """The QEMU command for ARCH, firmware included, with DISKS (-drive
    arguments) and a user-mode network."""
    common = ["-m", "1024", "-smp", "2", "-display", "none",
              "-vga", "none", "-monitor", "none", "-serial", "stdio",
              "-netdev", "user,id=n0", "-device", "virtio-net-pci,netdev=n0"] + disks
    if arch == "amd64":
        # UEFI with no display: the systems' images then talk on the
        # serial line, where with a BIOS they use the screen
        code = os.environ.get("GORK_OVMF_CODE", "/usr/share/OVMF/OVMF_CODE_4M.fd")
        vars_ = os.path.join(w, "OVMF_VARS.fd")
        shutil.copy(os.environ.get("GORK_OVMF_VARS", "/usr/share/OVMF/OVMF_VARS_4M.fd"), vars_)
        return ["qemu-system-x86_64", "-machine", "q35",
                "-drive", "if=pflash,format=raw,readonly=on,file=" + code,
                "-drive", "if=pflash,format=raw,file=" + vars_] + common
    fw = os.environ.get("GORK_AARCH64_FW", "/usr/share/qemu-efi-aarch64/QEMU_EFI.fd")
    return ["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a57",
            "-bios", fw] + common


def qemu(target, img, iso, w):
    arch = target.split("-")[1]
    if target.startswith("openbsd"):
        cd = ["-drive", "file=%s,if=virtio,format=raw,readonly=on" % iso]
    else:
        cd = ["-drive", "file=%s,media=cdrom,if=none,id=cd,readonly=on" % iso]
        cd += ["-device", "ide-cd,drive=cd"] if arch == "amd64" else \
              ["-device", "virtio-scsi-pci", "-device", "scsi-cd,drive=cd"]
    return machine(arch, w, ["-snapshot", "-drive", "file=%s,if=virtio,format=%s"
                             % (img, IMAGES[target][3])] + cd)


def openbsd_disk(target, media):
    """The OpenBSD system the test runs on: installed from MEDIA once, with
    INSTALL_CONF, then kept as GORK_VM/TARGET.qcow2."""
    import fcntl, http.server, threading
    arch = target.split("-")[1]
    disk = os.path.join(os.path.dirname(media), target + ".qcow2")
    if os.path.exists(disk):
        return disk
    # one install at a time in a cache that builds share; then look again,
    # another may have made the disk meanwhile
    lock = open(os.path.join(os.path.dirname(media), ".install.lock"), "w")
    fcntl.flock(lock, fcntl.LOCK_EX)
    if os.path.exists(disk):
        lock.close()
        return disk
    w = tempfile.mkdtemp(prefix="bsdinstall.")
    with open(os.path.join(w, "install.conf"), "w") as f:
        f.write(INSTALL_CONF % (PASSWORD, arch))

    class Quiet(http.server.SimpleHTTPRequestHandler):
        def __init__(self, *a, **k):
            super().__init__(*a, directory=w, **k)

        def log_message(self, *a):
            pass
    srv = http.server.ThreadingHTTPServer(("0.0.0.0", 0), Quiet)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    # a name of its own: a disk some earlier, killed, run left (and a QEMU
    # it left that may still hold it) must not get in the way
    new = "%s.new.%d" % (disk, os.getpid())
    subprocess.run(["qemu-img", "create", "-q", "-f", "qcow2", new, "6G"], check=True)
    log = open(os.path.join(w, "install.log"), "wb")
    c = Console(machine(arch, w, ["-drive", "file=%s,if=virtio,format=qcow2" % new,
                                  "-drive", "file=%s,if=virtio,format=raw,snapshot=on" % media]), log)
    try:
        if arch == "amd64":         # the installer's console on the serial line
            c.expect(b"boot>", timeout=300)
            c.send("set tty com0\r")
            time.sleep(1)
            c.send("boot\r")
        c.expect(b"(I)nstall, (U)pgrade, (A)utoinstall or (S)hell?", timeout=900)
        c.send("a\r")
        c.expect(b"Response file location?", timeout=300)
        c.send("http://10.0.2.2:%d/install.conf\r" % srv.server_address[1])
        if c.expect(b"CONGRATULATIONS", b"(I)nstall, (U)pgrade", timeout=3600) != b"CONGRATULATIONS":
            sys.exit("bsdtest: the OpenBSD install failed; its log is in " + log.name)
        time.sleep(10)              # the installer has synced; it reboots
    finally:
        # SIGTERM, not SIGKILL: QEMU then writes out the qcow2 metadata it
        # holds, without which the new disk is missing part of the install
        c.p.terminate()
        try:
            c.p.wait(120)
        except subprocess.TimeoutExpired:
            c.p.kill()
        srv.shutdown()
        log.close()
    os.rename(new, disk)
    shutil.copy(log.name, disk + ".install.log")
    shutil.rmtree(w)
    lock.close()
    return disk


class Console:
    def __init__(self, cmd, log):
        self.p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        self.buf = b""
        self.log = log

    def expect(self, *pats, timeout=600):
        end = time.time() + timeout
        while time.time() < end:
            for p in pats:
                i = self.buf.find(p)
                if i >= 0:
                    self.buf = self.buf[i + len(p):]
                    return p
            r, _, _ = select.select([self.p.stdout], [], [], 1)
            if r:
                d = os.read(self.p.stdout.fileno(), 65536)
                if not d:
                    break
                self.log.write(d)
                self.log.flush()
                self.buf += d
        raise SystemExit("bsdtest: timed out waiting for %r" % (pats,))

    def send(self, s):
        for ch in s.encode():           # slowly: serial consoles drop input
            self.p.stdin.write(bytes([ch]))
            self.p.stdin.flush()
            time.sleep(0.01)


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    target, package, fake = sys.argv[1:4]
    img = sys.argv[4] if len(sys.argv) > 4 else fetch(target)
    osname = target.split("-")[0]
    if osname == "openbsd":
        img = openbsd_disk(target, img)
    w = tempfile.mkdtemp(prefix="bsdtest.")
    cd = os.path.join(w, "cd")
    os.makedirs(cd)
    with tarfile.open(package) as t:
        m = [x for x in t.getmembers() if x.name.endswith("/gork")][0]
        with t.extractfile(m) as src, open(os.path.join(cd, "gork"), "wb") as dst:
            shutil.copyfileobj(src, dst)
    shutil.copy(fake, os.path.join(cd, "fakeapi"))
    for f in ("vmcheck.sh", "mcp.sh"):
        shutil.copy(os.path.join(HERE, f), os.path.join(cd, f))
    for f in os.listdir(cd):
        os.chmod(os.path.join(cd, f), 0o755)
    iso = os.path.join(w, "cd.iso")
    mk = shutil.which("xorriso") and ["xorriso", "-as", "mkisofs"] or ["genisoimage"]
    subprocess.run(mk + ["-quiet", "-R", "-o", iso, cd], check=True)

    log = open(os.path.join(w, "console.log"), "wb")
    c = Console(qemu(target, img, iso, w), log)
    try:
        for want, answer in BOOT.get(target, []):
            c.expect(want, timeout=300)
            c.send(answer)
        c.expect(b"login:", timeout=float(os.environ.get("GORK_BOOT_TIMEOUT", "900")))
        c.send("root\r")
        if c.expect(b"# ", b"$ ", b"assword:", timeout=120) == b"assword:":
            c.send(PASSWORD + "\r")
            c.expect(b"# ", timeout=120)
        # a prompt that cannot appear in the output
        c.send("PS1='@@ '; export PS1\r")
        c.expect(b"@@ ", timeout=60)
        c.send(MOUNT[osname] + " && cp /cd/* /tmp/ && chmod +x /tmp/gork /tmp/fakeapi\r")
        c.expect(b"@@ ", timeout=120)
        c.send("sh /tmp/vmcheck.sh /tmp/gork /tmp/fakeapi /tmp/mcp.sh; echo vmcheck=$?\r")
        r = c.expect(b"vmcheck=0", b"vmcheck=1", b"vmcheck=2", timeout=900)
        c.send(OFF[osname] + "\r")
        time.sleep(10)
    finally:
        c.p.kill()
        log.close()
    out = open(os.path.join(w, "console.log"), "rb").read().decode("latin-1")
    for line in out.replace("\r", "").splitlines():
        if line.startswith(("ok:", "FAIL:", "all checks passed")) or "check(s) failed" in line:
            print(line)
    if r != b"vmcheck=0":
        print("bsdtest: console log in " + log.name)
        sys.exit(1)
    shutil.rmtree(w)


if __name__ == "__main__":
    main()
