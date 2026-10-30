#!/usr/bin/env python3
"""Run gork from uefibuild.sh in the UEFI shell, in QEMU.

Usage:  python3 testing/uefitest.py PACKAGE FAKEAPI [MACHINE [SHELL]]

PACKAGE is gork-<version>-uefi.zip, FAKEAPI testing/fakeapi.c built for
this machine.  MACHINE is x64 (gorkx64.efi, on OVMF in a q35 machine; the
default), aa64 (gorkaa64.efi, on AAVMF in QEMU's virt machine), x64-own
(OVMF without its network stack, so on gork's own TCP/IP) or uboot
(gorkaa64.efi under U-Boot's UEFI, on gork's own TCP/IP).  SHELL is a
UEFI shell for it (default: Debian's efi-shell-x64 or -aa64).  A FAT
disk gets the shell as \\EFI\\BOOT\\BOOT<ARCH>.EFI, gork as gork.efi, its
gork.cnf and a startup.nsh that runs the checks; QEMU's user network takes
the guest to the fake API on 10.0.2.2.  The disk's files are read back
afterwards.  Prints "ok:" or "FAIL:" per check and exits 1 if any
failed.  Needs qemu-system-x86_64 and OVMF, or qemu-system-aarch64 and
qemu-efi-aarch64 (u-boot-qemu for uboot), mtools and dosfstools.

A -nonet PACKAGE (no network: docs/SERIAL.md) runs on x64 only, over the
machine's second serial port, COM2 (the console is COM1): a socat
pseudo-terminal pair takes it to $GORK_RELAY, run as `gork-relay -S`,
in front of the fake.  Needs socat then.
"""
import json, os, re, shutil, subprocess, sys, tempfile, time, zipfile

PORT = 8099

OVMF, AAVMF = "/usr/share/OVMF/OVMF_%s_4M.fd", "/usr/share/AAVMF/AAVMF_%s.fd"
NOPXE = ["-fw_cfg", "name=opt/org.tianocore/IPv4PXESupport,string=n",
         "-fw_cfg", "name=opt/org.tianocore/IPv6PXESupport,string=n"]
NIC = ["-nic", "user,model=virtio-net-pci"]

# MACHINE: the gork it runs (x64 or aa64), its firmware's code and
# variables (None: U-Boot, which keeps none), and the rest of QEMU's line.
# The firmware's network stack waits for a random number generator: a
# virtio one on x86-64; on arm64 the RNDR instruction of `-cpu max`, as a
# virtio one comes too late there.  Without one, OVMF has the network card
# but no TCP/IP, as a PC with its network stack turned off: gork's own
# stack (ip.c) is what runs then, and under U-Boot.
MACHINES = {
    "x64": ("x64", OVMF % "CODE", OVMF % "VARS",
            ["qemu-system-x86_64", "-machine", "q35", "-m", "512",
             "-device", "ide-hd,drive=esp,bus=ide.0,bootindex=0",
             "-device", "virtio-rng-pci"] + NOPXE + NIC),
    "x64-own": ("x64", OVMF % "CODE", OVMF % "VARS",
                ["qemu-system-x86_64", "-machine", "q35", "-m", "512",
                 "-device", "ide-hd,drive=esp,bus=ide.0,bootindex=0"] + NOPXE + NIC),
    "aa64": ("aa64", AAVMF % "CODE", AAVMF % "VARS",
             ["qemu-system-aarch64", "-M", "virt", "-cpu", "max", "-m", "1024",
              "-device", "virtio-blk-pci,drive=esp,bootindex=0"] + NOPXE + NIC),
    "uboot": ("aa64", None, None,
              ["qemu-system-aarch64", "-M", "virt", "-cpu", "max", "-m", "1024",
               "-bios", "/usr/lib/u-boot/qemu_arm64/u-boot.bin",
               "-device", "virtio-blk-pci,drive=esp"] + NIC),
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
    machine = sys.argv[3] if len(sys.argv) > 3 else "x64"
    arch, code, varstore, qemu = MACHINES[machine]
    nonet = "-nonet" in os.path.basename(pkg)
    relay = os.environ.get("GORK_RELAY")
    if nonet and (machine != "x64" or not relay):
        sys.exit("a -nonet package runs on x64, with $GORK_RELAY set")
    to = "serial:COM2:38400" if nonet else "10.0.2.2:%d" % PORT
    shell = sys.argv[4] if len(sys.argv) > 4 else \
        "/usr/share/efi-shell-%s/shell%s.efi" % (arch, arch)
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
        f.write("relay = %s\napi_key = sk-fake\ntimeout = 20\n"
                "retries = 0\n" % to)
    with open(os.path.join(esp, "startup.nsh"), "w", newline="\r\n") as f:
        f.write(SCRIPT.replace("10.0.2.2:%d" % PORT, to))
    with open(os.path.join(esp, "w", "abcdefghijkl.c"), "w") as f:
        f.write("old\n")

    img = os.path.join(t, "esp.img")
    sh("mkfs.fat", "-C", "-F", "16", "-n", "GORK", img, "65536",
       stdout=subprocess.DEVNULL)
    sh("mcopy", "-i", img, "-s", "-Q", *[os.path.join(esp, n) for n in os.listdir(esp)], "::/")
    flash = []
    if code is not None:
        vars_fd = os.path.join(t, "vars.fd")
        shutil.copy(varstore, vars_fd)
        flash = ["-drive", "if=pflash,format=raw,readonly=on,file=" + code,
                 "-drive", "if=pflash,format=raw,file=" + vars_fd]

    fp = subprocess.Popen([fake, str(PORT)], cwd=fdir, stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL)
    wire = []                           # socat and gork-relay, for COM2
    if nonet:
        com2, far = os.path.join(t, "com2"), os.path.join(t, "relay")
        wire = [subprocess.Popen(["socat", "pty,raw,echo=0,link=" + com2,
                                  "pty,raw,echo=0,link=" + far])]
        for _ in range(50):
            if os.path.exists(com2) and os.path.exists(far):
                break
            time.sleep(0.1)
        wire.append(subprocess.Popen([relay, "-p", "-v", "-t", "0", "-S",
                                      far + ":38400", "127.0.0.1:%d" % PORT],
                                     stderr=open(os.path.join(t, "relay.log"), "w")))
        qemu = qemu + ["-chardev", "serial,id=com2,path=" + os.path.realpath(com2),
                       "-serial", "mon:stdio", "-serial", "chardev:com2"]
    time.sleep(1)
    try:
        q = subprocess.run(
            qemu + flash + ["-nographic", "-no-reboot",
                            "-drive", "if=none,id=esp,format=raw,file=" + img],
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, timeout=900)
        out = q.stdout.decode("utf-8", "replace")
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or b"").decode("utf-8", "replace") + "\n(QEMU timed out)"
    finally:
        for p in [fp] + wire:
            p.terminate()
            p.wait()
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

    # what the tools returned: each tool_result's text, from the fake's log
    results = []
    for line in log.splitlines():
        try:
            for m in json.loads(line).get("messages", [])[-1:]:
                for b in m["content"] if isinstance(m["content"], list) else []:
                    if b.get("type") == "tool_result":
                        c = b.get("content")
                        results.append(c if isinstance(c, str) else json.dumps(c))
        except (ValueError, KeyError, TypeError):
            pass

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
        ("read_file", "a\nb\n" in results),
        ("list_dir", "abcdefghijkl.c" in log),
        ("run_command", "[exit " in log),
        ("paths outside the working directory refused",
         "outside the working directory" in log and read("..", "escape.txt") is None),
        ("a write in a subdirectory", read("sub", "w1.txt") == b"x"),
        ("a UTF-16 config, as the shell writes it", "PONG" in part("utf16")),
        ("the request timeout", "timeout on socket" in part("slow") or
         "timeout on the serial line" in part("slow")),
        ("exit status in lasterror", "==status 0x1" in out),
    ]
    fails = 0
    for name, good in checks:
        print(("ok: " if good else "FAIL: ") + name)
        fails += not good
    if fails and not os.environ.get("UEFITEST_VERBOSE"):
        print(out[-3000:])
    if fails and nonet:
        with open(os.path.join(t, "relay.log")) as f:
            print("gork-relay -S:\n" + f.read()[-4000:])
    if os.environ.get("UEFITEST_KEEP"):
        print("kept: " + t)
    else:
        shutil.rmtree(t, ignore_errors=True)
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
