#!/usr/bin/env python3
"""C source filter for the host / PS Vita port (after preproc).

1. Removes what only makes sense for the GBA's ARM compiler and does not
   change what the code computes:
     - register variables bound to ARM registers:  register u32 x asm("r4")
     - empty asm statements that only clobber ARM registers: asm("":::"r5")
2. Functions that exist only as Thumb assembly (NAKED, no C version in the
   decomp) are translated into C from the GBA build's object file with
   asm2c.py: the definition is replaced by a wrapper that calls the
   translated code, which is appended to the end of the file.

usage: naked2c.py <in.c> <out.c> [--obj gba_object.o --objdump arm-none-eabi-objdump]
"""
import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import asm2c as A  # noqa: E402

# libgcc helpers the Thumb code calls (arguments in r0, r1).
LIBGCC = {
    "__divsi3": "r0 = (u32)((s32)r0 / (s32)r1);",
    "__modsi3": "r0 = (u32)((s32)r0 % (s32)r1);",
    "__udivsi3": "r0 = r0 / r1;",
    "__umodsi3": "r0 = r0 % r1;",
}


class GameFuncGen(A.FuncGen):
    def do(self, ins, base, ops, cond):
        if base == "bl":
            name = self.tr.relocs.get(ins.addr, (None,))[0]
            if name is None:  # call inside the same object, resolved by the assembler
                tgt = int(ops[0].split()[0], 16)
                name = self.tr.funcs.get(tgt)
                if name is None:
                    A.die("bl to unknown local target at 0x%x" % ins.addr)
            if name in LIBGCC:
                self.emit(LIBGCC[name])
            elif name.startswith("_call_via_"):
                r = A.reg(name[len("_call_via_"):])
                self.emit("r0 = ASMRT_CALL(%s, r0, r1, r2, r3);" % r)
            elif name in self.internal:
                self.emit("r0 = asmfn_%s(r0, r1, r2, r3);" % name)
            else:
                self.tr.data_refs.add(name)
                self.emit("r0 = ASMRT_CALL(%s, r0, r1, r2, r3);" % name)
            return
        return super().do(ins, base, ops, cond)

    def generate(self):
        out = super().generate()
        out[0] = "static " + out[0]
        return out


class GameTr(A.Translator):
    def __init__(self, obj, objdump):
        super().__init__(obj, objdump)
        # Relocations against a section symbol (static data of the file):
        # find the named symbol the addend points into.
        from elftools.elf.elffile import ELFFile
        from elftools.elf.relocation import RelocationSection
        self.sec_relocs = {}
        with open(obj, "rb") as f:
            elf = ELFFile(f)
            symtab = elf.get_section_by_name(".symtab")
            by_sec = {}
            for sym in symtab.iter_symbols():
                if sym.name and not sym.name.startswith("$") and sym["st_info"]["type"] in ("STT_OBJECT", "STT_NOTYPE", "STT_FUNC") \
                        and isinstance(sym["st_shndx"], int):
                    by_sec.setdefault(sym["st_shndx"], []).append((sym["st_value"], sym.name))
            for rs in elf.iter_sections():
                if isinstance(rs, RelocationSection) and rs.name == ".rel.text":
                    for r in rs.iter_relocations():
                        sym = symtab.get_symbol(r["r_info_sym"])
                        if sym["st_info"]["type"] == "STT_SECTION":
                            self.sec_relocs[r["r_offset"]] = sorted(by_sec.get(sym["st_shndx"], []))

    def word_at(self, addr):
        if addr in self.relocs:
            name, rtype = self.relocs[addr]
            if rtype != 2:
                A.die("unexpected relocation type at pool 0x%x" % addr)
            addend = int.from_bytes(self.text[addr:addr + 4], "little")
            if addr in self.sec_relocs:
                cands = [(v, n) for v, n in self.sec_relocs[addr] if v <= addend]
                if not cands:
                    A.die("no symbol for section-relative reference at 0x%x" % addr)
                v, name = cands[-1]
                addend -= v
            self.data_refs.add(name)
            return "((u32)(uintptr_t)&(%s) + %du)" % (name, addend)
        return "0x%08xu" % int.from_bytes(self.text[addr:addr + 4], "little")


STRUCT_RE = re.compile(r'\b(struct|union)\b((?:\s+__attribute__\s*\(\([^)]*\)\))*)(\s+[A-Za-z_]\w*)?\s*\{')
LINE_RE = re.compile(r'^#\s*\d+\s+"([^"]*)"', re.M)


def apcs_struct_layout(src):
    """The GBA build uses the old ARM APCS ABI: every struct and union is
    aligned to 4 bytes and its size rounded up to a multiple of 4. The data
    assembly and the assembly-only functions depend on that layout, so the
    port gives the game's own structs (defined in include/ and src/) the same
    layout with aligned(4). Packed structs keep their layout."""
    # which file each part of the preprocessed text comes from
    marks = [(m.start(), m.group(1)) for m in LINE_RE.finditer(src)]

    def game_file(pos):
        f = ""
        lo, hi = 0, len(marks) - 1
        while lo <= hi:
            mid = (lo + hi) // 2
            if marks[mid][0] <= pos:
                f = marks[mid][1]
                lo = mid + 1
            else:
                hi = mid - 1
        return f.startswith(("include/", "src/", "./include/", "./src/"))

    out, last = [], 0
    for m in STRUCT_RE.finditer(src):
        if "packed" in m.group(2) or not game_file(m.start()):
            continue
        brace = m.end() - 1
        try:
            end = match_brace(src, brace)
        except ValueError:
            continue
        if re.match(r'\s*__attribute__\s*\(\(\s*packed', src[end:end + 64]):
            continue
        out.append(src[last:m.start()])
        out.append("%s __attribute__((aligned(4)))%s%s {" % (m.group(1), m.group(2), m.group(3) or ""))
        last = m.end()
    out.append(src[last:])
    return "".join(out)


def strip_arm_only(src):
    # top-level asm(".include ...") lines only feed the assembly functions
    src = re.sub(r'(?m)^\s*asm\s*\(\s*"\.include[^;]*\)\s*;', '', src)
    src = re.sub(r'(\bregister\b[^;=(){}]*?)\s*(?:asm|__asm__)\s*\(\s*"r\d+"\s*\)', r'\1', src)
    src = re.sub(r'\b(?:asm|__asm__)\s*(?:volatile\s*)?\(\s*""\s*:::\s*"[^"]*"(?:\s*,\s*"[^"]*")*\s*\)\s*;', ';', src)
    return src


def match_brace(s, i):
    """s[i] == '{'; returns the index after the matching '}' (strings/chars skipped)."""
    depth = 0
    n = len(s)
    while i < n:
        ch = s[i]
        if ch == '"' or ch == "'":
            q = ch
            i += 1
            while i < n and s[i] != q:
                if s[i] == "\\":
                    i += 1
                i += 1
        elif ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    raise ValueError("unbalanced braces")


NAKED_RE = re.compile(r'__attribute(?:__)?\s*\(\(\s*naked\s*\)\)\s*')
HEADER_RE = re.compile(r'^\s*((?:static\s+)?)([A-Za-z_][\w\s\*]*?)\s*\b([A-Za-z_]\w*)\s*\(([^)]*)\)\s*$', re.S)


def params_of(plist):
    plist = plist.strip()
    if plist == "" or plist == "void":
        return []
    out = []
    for i, p in enumerate(plist.split(",")):
        p = p.strip()
        m = re.match(r"^(.*?)([A-Za-z_]\w*)$", p)
        if m and m.group(1).strip() and not m.group(2) in ("u8", "u16", "u32", "s8", "s16", "s32", "int", "bool8"):
            out.append((m.group(1).strip(), m.group(2)))
        else:
            out.append((p, "p%d" % i))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("inp")
    ap.add_argument("out")
    ap.add_argument("--obj")
    ap.add_argument("--objdump", default="arm-none-eabi-objdump")
    args = ap.parse_args()

    src = apcs_struct_layout(strip_arm_only(open(args.inp, encoding="latin-1").read()))
    nakeds = []
    pos = 0
    while True:
        m = NAKED_RE.search(src, pos)
        if not m:
            break
        brace = src.index("{", m.end())
        header = src[m.end():brace]
        hm = HEADER_RE.match(header)
        if not hm:
            sys.exit("naked2c: cannot parse header %r" % header)
        end = match_brace(src, brace)
        nakeds.append((m.start(), end, hm.group(1).strip(), hm.group(2).strip(), hm.group(3), hm.group(4)))
        pos = end

    if not nakeds:
        open(args.out, "w", encoding="latin-1").write(src)
        return
    if not args.obj:
        sys.exit("naked2c: %s has NAKED functions but no GBA object was given" % args.inp)

    tr = GameTr(args.obj, args.objdump)
    tr.data_refs = set()
    funcs = {name: insns for name, _, insns in tr.functions()}
    names = {n[4] for n in nakeds}
    missing = names - set(funcs)
    if missing:
        sys.exit("naked2c: %s not found in %s" % (", ".join(sorted(missing)), args.obj))

    out_parts, last = [], 0
    decls = []
    for start, end, static, ret, name, plist in nakeds:
        params = params_of(plist)
        decls.append("static u32 asmfn_%s(u32 a0, u32 a1, u32 a2, u32 a3);" % name)
        pdecl = ", ".join("%s %s" % (t, n) for t, n in params) or "void"
        callargs = ["(u32)(uintptr_t)%s" % n if "*" in t else "(u32)%s" % n for t, n in params][:4]
        if len(params) > 4:
            sys.exit("naked2c: %s has more than 4 parameters" % name)
        callargs += ["0"] * (4 - len(callargs))
        call = "asmfn_%s(%s)" % (name, ", ".join(callargs))
        if ret == "void":
            body = "{\n    %s;\n}\n" % call
        elif "*" in ret:
            body = "{\n    return (%s)(uintptr_t)%s;\n}\n" % (ret, call)
        else:
            body = "{\n    return (%s)%s;\n}\n" % (ret, call)
        wrapper = "static u32 asmfn_%s(u32, u32, u32, u32);\n%s%s %s(%s)\n%s" % (
            name, (static + " ") if static else "", ret, name, pdecl, body)
        out_parts.append(src[last:start])
        out_parts.append("/* port: %s was assembly only; translated by tools/port/naked2c.py */\n" % name)
        out_parts.append(wrapper)
        last = end
    out_parts.append(src[last:])
    text = "".join(out_parts)

    gen = []
    for name in sorted(names):
        try:
            g = GameFuncGen(tr, name, funcs[name], names)
            gen += g.generate() + [""]
        except SystemExit as e:
            # Not translatable (e.g. calls into the middle of another
            # function). Only acceptable for code the game never runs: the
            # stub stops the port with a message if it ever is.
            sys.stderr.write("naked2c: %s: %s not translated (%s), stubbed\n" % (args.inp, name, e))
            gen += ["void PortFatalHalt(const char *why);",
                    "static u32 asmfn_%s(u32 a0, u32 a1, u32 a2, u32 a3)" % name,
                    "{", '    PortFatalHalt("untranslated assembly function %s");' % name, "    return 0;", "}", ""]
    # Symbols the assembly uses that the C file never declares.
    externs = []
    for ref in sorted(tr.data_refs):
        if not re.search(r"\b%s\b" % re.escape(ref), text):
            externs.append("extern char %s[];" % ref)
    rt = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "port", "asm_rt.h")).read()
    rt = rt.replace("#include <stdint.h>", "")  # the file is already preprocessed
    tail = ["", "/* ---- translated assembly (tools/port/naked2c.py) ---- */", rt] + externs + gen
    open(args.out, "w", encoding="latin-1").write(text + "\n".join(tail) + "\n")


if __name__ == "__main__":
    main()
