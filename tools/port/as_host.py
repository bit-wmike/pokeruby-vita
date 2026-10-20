#!/usr/bin/env python3
"""Makes the game's data assembly (scripts, maps, songs, ...) assemble for
the host (x86, `as --32`). The data files contain no ARM code, only data
directives; the differences are directive syntax:
  - `.align N` is a power of two on ARM but a byte count on x86 -> .p2align
  - .thumb/.arm/.thumb_func/.syntax/.code are dropped
  - `%function` / `%object` in .type -> `@function` / `@object`
`.include` files are inlined (they contain the same directives).
For the PS Vita (ARM) the original files assemble unchanged.
usage: as_host.py [--arm] <in.s> <out.s> -I dir [-I dir ...]
"""
import os
import re
import sys

args = sys.argv[1:]
arm = "--arm" in args
args = [a for a in args if a != "--arm"]
inp, out = args[0], args[1]
if arm:  # ARM targets assemble the original syntax
    open(out, "w", encoding="latin-1").write(open(inp, encoding="latin-1").read())
    sys.exit(0)
dirs = [args[i + 1] for i in range(2, len(args) - 1) if args[i] == "-I"]

def find(name, base):
    for d in [os.path.dirname(base), "."] + dirs:
        p = os.path.join(d, name)
        if os.path.exists(p):
            return p
    sys.exit("as_host: cannot find include %s" % name)

def strip_comment(line):
    """Drops an ARM '@' line comment (outside of string literals)."""
    q = None
    for i, ch in enumerate(line):
        if q:
            if ch == "\\":
                continue
            if ch == q:
                q = None
        elif ch in "\"'":
            q = ch
        elif ch == "@" and not (i > 0 and line[i - 1] == "\\"):  # \@ is the macro counter
            return line[:i]
    return line


def convert(text, path, outl, depth=0):
    for line in text.splitlines():
        m = re.match(r'\s*\.include\s+"([^"]+)"', line)
        if m:
            p = find(m.group(1), path)
            convert(open(p, encoding="latin-1").read(), p, outl, depth + 1)
            continue
        if re.match(r'\s*\.(thumb|arm|thumb_func|syntax|code|cpu|fpu)\b', line):
            continue
        line = strip_comment(line)
        line = re.sub(r'^(\s*(?:[A-Za-z_0-9.]+:\s*)?)\.align\b', r'\1.p2align', line)
        line = re.sub(r'%(function|object)\b', r'@\1', line)
        line = re.sub(r'(^|\s)\.word\b', r'\1.4byte', line)  # .word is 4 bytes on ARM, 2 on x86
        outl.append(line)

outl = []
convert(open(inp, encoding="latin-1").read(), inp, outl)
open(out, "w", encoding="latin-1").write("\n".join(outl) + "\n")
