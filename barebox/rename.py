#!/usr/bin/env python3
"""Write the header that renames the C library for gork's half of the
barebox build.

barebox has its own printf, strlen, open and the rest, which gork's
(fwlibc.c, barebox/libc.c) must not meet at the link; so in gork's files
every name the standard headers (fwinclude/) declare becomes gork_NAME.

Usage:  python3 barebox/rename.py CPP OUT     (CPP: the preprocessor, as
        "aarch64-linux-gnu-gcc -E"; run from the source tree)
"""
import os, re, subprocess, sys

cpp, out = sys.argv[1].split(), sys.argv[2]
headers = []
for d, _, files in os.walk("fwinclude"):
    headers += [os.path.relpath(os.path.join(d, f), "fwinclude")
                for f in files if f.endswith(".h")]
src = "".join("#include <%s>\n" % h for h in sorted(headers))
# the compiler's own headers (stdarg.h, stddef.h) stay; the system's go
inc = subprocess.run(cpp[:1] + ["-print-file-name=include"], capture_output=True,
                     text=True, check=True).stdout.strip()
r = subprocess.run(cpp + ["-P", "-ffreestanding", "-nostdinc", "-isystem", inc, "-Ifwinclude",
                          "-x", "c", "-"], input=src, capture_output=True, text=True)
if r.returncode != 0:
    sys.exit(r.stderr)
text = r.stdout

# The declarations at file scope: functions and extern objects, not types.
names = set()
depth = 0
stmt = ""
for ch in text:
    if ch == "{":
        depth += 1
    elif ch == "}":
        depth -= 1
    elif depth == 0:
        stmt += ch
        if ch == ";":
            s = " ".join(stmt.split())
            stmt = ""
            # void exit(int) __attribute__((noreturn)); is still exit()
            s = re.sub(r"__attribute__\s*\(\((?:[^()]|\([^()]*\))*\)\)", "", s)
            s = " ".join(s.split())
            if s.startswith("typedef") or s.startswith("struct") and "(" not in s:
                continue
            m = re.search(r"\b([A-Za-z_]\w*)\s*\((?:[^()]|\([^()]*\))*\)\s*;$", s)
            if m and not re.search(r"\(\s*\*\s*%s\s*\)" % m.group(1), s):
                names.add(m.group(1))
                continue
            if s.startswith("extern"):      # extern FILE *stdin, *stdout;
                for d in s[:-1].split(","):
                    m = re.search(r"([A-Za-z_]\w*)\s*(\[[^\]]*\])?\s*$", d)
                    if m:
                        names.add(m.group(1))
names = {n for n in names if not n.startswith("__")}
names -= {"if", "while", "for", "return", "sizeof"}
names |= {"main"}               # gork's main() is the gork command's
with open(out, "w") as f:
    f.write("/* Made by barebox/rename.py from fwinclude/: do not edit. */\n")
    f.write("#ifndef GORK_RENAME_H\n#define GORK_RENAME_H\n")
    for n in sorted(names):
        f.write("#define %s gork_%s\n" % (n, n))
    f.write("#endif\n")
print("%s: %d names" % (out, len(names)))
