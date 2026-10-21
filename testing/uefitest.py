#!/usr/bin/env python3
"""Run gork from uefibuild.sh in the UEFI shell, in QEMU.

Usage:  python3 testing/uefitest.py PACKAGE FAKEAPI [ARCH [SHELL]]

PACKAGE is gork-<version>-uefi.zip, FAKEAPI testing/fakeapi.c built for
this machine, ARCH x64 (gorkx64.efi, on OVMF in a q35 machine; the
default) or aa64 (gorkaa64.efi, on AAVMF in QEMU's virt machine), SHELL
a UEFI shell for it (default: Debian's efi-shell-x64 or -aa64).  A FAT
disk gets the shell as \\EFI\\BOOT\\BOOT<ARCH>.EFI, gork as gork.efi, its
gork.cnf and a startup.nsh that runs the checks; QEMU's user network takes
the guest to the fake API on 10.0.2.2.  The disk's files are read back
afterwards.  Prints "ok:" or "FAIL:" per check and exits 1 if any
failed.  Needs qemu-system-x86_64 and OVMF, or qemu-system-aarch64 and
qemu-efi-aarch64, mtools and dosfstools.
"""
import os, shutil, subprocess, sys, tempfile, time, zipfile

PORT = 8099

# ARCH: the firmware's code and variables, and how QEMU boots it
MACHINES = {
    "x64": ("/usr/share/OVMF/OVMF_CODE_4M.fd", "/usr/share/OVMF/OVMF_VARS_4M.fd",
            ["qemu-system-x86_64", "-machine", "q35", "-m", "512"],
            "ide-hd,drive=esp,bus=ide.0,bootindex=0"),
    "aa64": ("/usr/share/AAVMF/AAVMF_CODE.fd", "/usr/share/AAVMF/AAVMF_VARS.fd",
             # "max": its RNDR instruction is the random number generator
             # the network stack waits for; a virtio one comes too late here
             ["qemu-system-aarch64", "-M", "virt", "-cpu", "max", "-m", "1024"],
             "virtio-blk-pci,drive=esp,bootindex=0"),
}

# Each check: a line of startup.nsh, and what the console must show after
# it.  The rest is checked in the fake's log and on the disk afterwards.
SCRIPT = r"""@echo -off
fs0:
cd \w
echo ==version
fs0:\gork.efi --version
echo ==notui
fs0:\gork.efi
echo ==hello
fs0:\gork.efi hello
echo ==go
set GORK_ALLOW_RUN 1
fs0:\gork.efi go
set -d GORK_ALLOW_RUN
echo ==esc
fs0:\gork.efi esc
echo ==sub
cd sub
fs0:\gork.efi w1
cd \w
echo ==utf16
echo relay = 10.0.2.2:%d > fs0:\u16.cnf
echo api_key = sk-fake >> fs0:\u16.cnf
set GORK_CONFIG fs0:\u16.cnf
fs0:\gork.efi hello
set -d GORK_CONFIG
echo ==slow
fs0:\gork.efi slow
echo ==status %%lasterror%%
echo ==done
reset -s
""" % PORT


def sh(*cmd, **kw):
    subprocess.run(cmd, check=True, **kw)


def main():
    if len(sys.argv) not in (3, 4, 5) or \
            (len(sys.argv) > 3 and sys.argv[3] not in MACHINES):
        sys.exit(__doc__)
    pkg, fake = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
    arch = sys.argv[3] if len(sys.argv) > 3 else "x64"
    shell = sys.argv[4] if len(sys.argv) > 4 else \
        "/usr/share/efi-shell-%s/shell%s.efi" % (arch, arch)
    code, varstore, qemu, disk = MACHINES[arch]
    t = tempfile.mkdtemp(prefix="uefitest.")
    esp, back, fdir = (os.path.join(t, d) for d in ("esp", "back", "f"))
    for d in (esp, back, fdir, os.path.join(esp, "EFI", "BOOT"),
              os.path.join(esp, "w", "sub")):
        os.makedirs(d, exist_ok=True)
    with zipfile.ZipFile(pkg) as z:
        with z.open("gork%s.efi" % arch) as src, \
                open(os.path.join(esp, "gork.efi"), "wb") as dst:
            shutil.copyfileobj(src, dst)
    shutil.copy(shell, os.path.join(esp, "EFI", "BOOT", "BOOT%s.EFI" % arch.upper()))
    with open(os.path.join(esp, "gork.cnf"), "w", newline="\r\n") as f:
        f.write("relay = 10.0.2.2:%d\napi_key = sk-fake\ntimeout = 20\n"
                "retries = 0\n" % PORT)
    with open(os.path.join(esp, "startup.nsh"), "w", newline="\r\n") as f:
        f.write(SCRIPT)
    with open(os.path.join(esp, "w", "abcdefghijkl.c"), "w") as f:
        f.write("old\n")

    img = os.path.join(t, "esp.img")
    sh("mkfs.fat", "-C", "-F", "16", "-n", "GORK", img, "65536",
       stdout=subprocess.DEVNULL)
    sh("mcopy", "-i", img, "-s", "-Q", *[os.path.join(esp, n) for n in os.listdir(esp)], "::/")
    vars_fd = os.path.join(t, "vars.fd")
    shutil.copy(varstore, vars_fd)

    fp = subprocess.Popen([fake, str(PORT)], cwd=fdir, stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL)
    time.sleep(1)
    try:
        q = subprocess.run(
            qemu + ["-nographic", "-no-reboot",
             "-drive", "if=pflash,format=raw,readonly=on,file=" + code,
             "-drive", "if=pflash,format=raw,file=" + vars_fd,
             "-drive", "if=none,id=esp,format=raw,file=" + img,
             "-device", disk,
             # no network boot before the disk; the firmware's network
             # stack wants a random number generator
             "-fw_cfg", "name=opt/org.tianocore/IPv4PXESupport,string=n",
             "-fw_cfg", "name=opt/org.tianocore/IPv6PXESupport,string=n",
             "-device", "virtio-rng-pci",
             "-nic", "user,model=virtio-net-pci"],
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, timeout=900)
        out = q.stdout.decode("utf-8", "replace")
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or b"").decode("utf-8", "replace") + "\n(QEMU timed out)"
    finally:
        fp.terminate()
        fp.wait()
    sh("mcopy", "-i", img, "-s", "-n", "-Q", "::/*", back)
    out = out.replace("\r", "")
    if os.environ.get("UEFITEST_VERBOSE"):
        print(out)

    def part(name):             # the console between ==name and the next ==
        i = out.find("==" + name)
        if i < 0:
            return ""
        j = out.find("\n==", i + 1)
        return out[i:j if j >= 0 else len(out)]

    log = ""
    if os.path.exists(os.path.join(fdir, "fakeapi.log")):
        with open(os.path.join(fdir, "fakeapi.log")) as f:
            log = f.read()

    def read(*p):
        try:
            with open(os.path.join(back, "w", *p), "rb") as f:
                return f.read()
        except OSError:
            return None

    checks = [
        ("--version", "gork " in part("version")),
        ("batch mode only", "no TUI" in part("notui")),
        ("a reply", "PONG" in part("hello")),
        ("write_file", read("out.txt") == b"a\r\nb\r\n"),
        ("read_file", '"content":"a\\nb\\n"' in log),
        ("list_dir", "abcdefghijkl.c" in log),
        ("run_command", "[exit " in log),
        ("paths outside the working directory refused",
         "outside the working directory" in log and read("..", "escape.txt") is None),
        ("a write in a subdirectory", read("sub", "w1.txt") == b"x"),
        ("a UTF-16 config, as the shell writes it", "PONG" in part("utf16")),
        ("the request timeout", "timeout on socket" in part("slow")),
        ("exit status in lasterror", "==status 0x1" in out),
    ]
    fails = 0
    for name, good in checks:
        print(("ok: " if good else "FAIL: ") + name)
        fails += not good
    if fails and not os.environ.get("UEFITEST_VERBOSE"):
        print(out[-3000:])
    shutil.rmtree(t, ignore_errors=True)
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
