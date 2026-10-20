#!/usr/bin/env python3
"""Translates a whole hand-written ARM/Thumb object (the m4a sound engine,
src/libs/m4a_1.s) into one C function, for the host / PS Vita port.

Hand-written assembly jumps between "functions", calls local subroutines
and returns through saved registers, so it is not split into C functions:
all code lives in one C function with one register file. Calls inside the
object store a return token in lr and jump; `bx`/`pop {pc}` dispatch on
the token. A register that holds no token is a C function pointer: it is
called like a C function (r0-r3 in, r0 out) and execution continues at lr.
Every global function of the object gets a C entry point.

The instruction semantics come from asm2c.py (exact ARM7TDMI flags).

usage: asm2c_flat.py --obj m4a_1.o --out m4a_gen.c [--objdump ...]
       [--map SYMBOL=CFUNC ...]   pool references to SYMBOL become &CFUNC
"""
import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import asm2c as A  # noqa: E402
import naked2c as N  # noqa: E402

TOKEN_BASE = 0xF0000000
TOKEN_EXIT = TOKEN_BASE


class FlatGen(A.FuncGen):
    def __init__(self, tr, insns, labels, entries, symmap):
        super().__init__(tr, "flat", insns, set())
        self.labels = labels      # address -> name (all code symbols)
        self.entries = entries    # name -> address (callable from C)
        self.symmap = symmap
        self.ret_points = []      # return token -> address after the bl
        self.externs = set()

    def target_of_bl(self, ins, ops):
        name = self.tr.relocs.get(ins.addr, (None,))[0]
        if name is None:
            return int(ops[0].split()[0], 16), None
        if name in self.tr.symaddr:
            return self.tr.symaddr[name], None
        return None, name

    def call_external(self, expr):
        self.emit("r0 = ASMRT_CALL(%s, r0, r1, r2, r3);" % expr)

    def do(self, ins, base, ops, cond):
        e = self.emit
        if base == "bl":
            addr, ext = self.target_of_bl(ins, ops)
            if ext is not None:
                if ext in N.LIBGCC:
                    e(N.LIBGCC[ext])
                else:
                    self.externs.add(ext)
                    self.call_external(ext)
                return
            tok = len(self.ret_points) + 1
            self.ret_points.append(ins.addr + ins.size)
            self.targets.add(addr)
            e("lr = 0x%08xu; goto L_%x;" % (TOKEN_BASE + tok, addr))
            return
        if base == "bx":
            r = A.reg(ops[0])
            if r in self.adr_const:
                tgt = self.adr_const[r] & ~1
                self.targets.add(tgt)
                e("goto L_%x;" % tgt)
            else:
                e("t_ = %s; goto dispatch;" % r)
            return
        if base in ("pop", "ldmfd") or (base in ("ldmia", "ldm") and ops[0] == "sp!"):
            lst = A.reglist(ops[-1])
            if "pc" in lst:
                for i, rr in enumerate(lst):
                    if rr == "pc":
                        e("t_ = LD32(sp + %d);" % (4 * i))
                    else:
                        e("%s = LD32(sp + %d);" % (rr, 4 * i))
                e("sp += %d;" % (4 * len(lst)))
                e("goto dispatch;")
                return
        if base == "mov" and len(ops) == 2 and A.reg(ops[0]) == "pc":
            e("t_ = %s; goto dispatch;" % A.reg(ops[1]))
            return
        return super().do(ins, base, ops, cond)


class FlatTr(N.GameTr):
    def __init__(self, obj, objdump, symmap):
        super().__init__(obj, objdump)
        self.symmap = symmap
        self.ptr_entries = set()  # own functions whose address the code loads
        from elftools.elf.elffile import ELFFile
        self.symaddr = {}
        self.globals = {}
        with open(obj, "rb") as f:
            elf = ELFFile(f)
            text_idx = [i for i, s in enumerate(elf.iter_sections()) if s.name == ".text"][0]
            for s in elf.get_section_by_name(".symtab").iter_symbols():
                if s["st_shndx"] == text_idx and s.name and not s.name.startswith("$"):
                    self.symaddr[s.name] = s["st_value"] & ~1
                    if s["st_info"]["bind"] == "STB_GLOBAL" and s["st_info"]["type"] == "STT_FUNC":
                        self.globals[s.name] = s["st_value"] & ~1

    def word_at(self, addr):
        if addr in self.relocs:
            name, rtype = self.relocs[addr]
            addend = int.from_bytes(self.text[addr:addr + 4], "little")
            if name in self.symmap:
                self.ptr_entries.add(self.symmap[name])
                return "((u32)(uintptr_t)&(%s))" % self.symmap[name]
            if name in self.globals:  # a function of this object: its C entry
                self.ptr_entries.add(name)
                return "((u32)(uintptr_t)&(%s))" % name
            if name in self.symaddr or name == ".text" or (addr in self.sec_relocs and not self.sec_relocs[addr]):
                base = self.symaddr.get(name, 0)
                return "((u32)(uintptr_t)&gFlatText[%d])" % (base + addend)
        return super().word_at(addr)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--obj", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--objdump", default="arm-none-eabi-objdump")
    ap.add_argument("--map", action="append", default=[])
    args = ap.parse_args()
    symmap = dict(m.split("=") for m in args.map)

    tr = FlatTr(args.obj, args.objdump, symmap)
    tr.data_refs = set()
    labels = {a: n for n, a in tr.symaddr.items()}
    entries = dict(tr.globals)
    g = FlatGen(tr, tr.insns, labels, entries, symmap)
    for name, a in entries.items():
        g.targets.add(a)

    body = []
    prev_adr = False
    for ins in tr.insns:
        # An adr constant is only valid for the bx right after it.
        if not prev_adr:
            g.adr_const = {}
        g.lines = []
        g.translate(ins)
        prev_adr = any("/* adr " in l for l in g.lines)
        body.append((ins.addr, ins, g.lines))
    addrs = {a for a, _, _ in body}
    for t in g.targets | set(g.ret_points):
        if t not in addrs:
            A.die("target 0x%x is not an instruction" % t)

    out = ["/* Generated by tools/port/asm2c_flat.py from %s - do not edit. */" % os.path.basename(args.obj),
           "#include <stdint.h>",
           "typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;",
           "typedef int8_t s8; typedef int16_t s16; typedef int32_t s32;",
           '#include "port/asm_rt.h"', ""]
    text = tr.text
    out.append("static const unsigned char gFlatText[%d] __attribute__((aligned(4))) = {" % len(text))
    for i in range(0, len(text), 16):
        out.append("    " + ", ".join("0x%02x" % b for b in text[i:i + 16]) + ",")
    out.append("};")
    refs = (tr.data_refs | g.externs)
    for r in sorted(refs - set(entries)):
        out.append("extern char %s[];" % r)
    for name in sorted(entries):
        out.append("u32 %s(u32, u32, u32, u32);" % name)
    out.append("")
    out.append("static u32 FlatRun(u32 entry, u32 a0, u32 a1, u32 a2, u32 a3)")
    out.append("{")
    out.append("    u32 r0 = a0, r1 = a1, r2 = a2, r3 = a3, r4 = 0, r5 = 0, r6 = 0, r7 = 0;")
    out.append("    u32 r8 = 0, r9 = 0, r10 = 0, r11 = 0, r12 = 0, lr = 0x%08xu, t_;" % TOKEN_EXIT)
    out.append("    u32 n = 0, z = 0, c = 0, v = 0;")
    out.append("    u32 stack_[512];")
    out.append("    u32 sp = (u32)(uintptr_t)&stack_[512];")
    out.append("    (void)n; (void)z; (void)c; (void)v; (void)r12;")
    out.append("    t_ = entry;")
    out.append("    goto enter;")
    out.append("dispatch:")
    out.append("    /* jumps to this object's own functions keep the register state */")
    for name in sorted(set(entries) | tr.ptr_entries):
        out.append("    if ((t_ & ~1u) == (u32)(uintptr_t)&%s) goto L_%x;" % (name, entries[name]))
    out.append("    if ((t_ & 0xF0000000u) != 0x%08xu) {" % TOKEN_BASE)
    out.append("        /* a C function pointer: call it, then return to lr */")
    out.append("        r0 = ASMRT_CALL(t_ & ~1u, r0, r1, r2, r3);")
    out.append("        t_ = lr;")
    out.append("        if ((t_ & 0xF0000000u) != 0x%08xu)" % TOKEN_BASE)
    out.append("            return r0; /* never happens: lr always holds a token here */")
    out.append("    }")
    out.append("    switch (t_ & 0x0FFFFFFFu) {")
    out.append("        case 0: return r0;")
    for i, a in enumerate(g.ret_points):
        out.append("        case %d: goto L_%x;" % (i + 1, a))
    out.append("    }")
    out.append("    return r0;")
    out.append("enter:")
    out.append("    switch (t_) {")
    for i, (name, a) in enumerate(sorted(entries.items())):
        out.append("        case %d: goto L_%x; /* %s */" % (i, a, name))
    out.append("    }")
    out.append("    return 0;")
    targets = g.targets | set(g.ret_points)
    for addr, ins, lines in body:
        if addr in targets:
            out.append("L_%x:" % addr)
        out.append("    /* %04x %s %s */" % (addr, ins.mnem, ", ".join(ins.ops)))
        out += lines
    out.append("    return r0;")
    out.append("}")
    out.append("")
    for i, (name, a) in enumerate(sorted(entries.items())):
        out.append("u32 %s(u32 a0, u32 a1, u32 a2, u32 a3) { return FlatRun(%d, a0, a1, a2, a3); }" % (name, i))
    open(args.out, "w").write("\n".join(out) + "\n")
    print("asm2c_flat: %d instructions, %d entries, %d call sites" % (len(body), len(entries), len(g.ret_points)))


if __name__ == "__main__":
    main()
