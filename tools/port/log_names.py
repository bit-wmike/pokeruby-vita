#!/usr/bin/env python3
"""Adds function names to the addresses in a log (log.txt of the Vita or a
host log): every 0x... that points into a function of the ELF gets <name>.

    python3 tools/port/log_names.py log.txt build/port-vita-sapphire/pokemon_sapphire_vita-0.3.18.elf > log_named.txt

Use the ELF of the same version as the log (it is kept next to each VPK).
Needs pyelftools (pip install pyelftools).
"""
import bisect
import re
import sys

from elftools.elf.elffile import ELFFile


def load_symbols(path):
    syms = []
    with open(path, 'rb') as f:
        elf = ELFFile(f)
        for sec in elf.iter_sections():
            if sec.header.sh_type != 'SHT_SYMTAB':
                continue
            for s in sec.iter_symbols():
                if s['st_info']['type'] == 'STT_FUNC' and s['st_value']:
                    syms.append((s['st_value'] & ~1, s['st_size'], s.name))
    syms.sort()
    return syms


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    syms = load_symbols(sys.argv[2])
    starts = [s[0] for s in syms]

    def name(m):
        addr = int(m.group(0), 16) & ~1
        i = bisect.bisect_right(starts, addr) - 1
        if i >= 0:
            start, size, n = syms[i]
            if addr < start + max(size, 1):
                off = addr - start
                return '%s<%s%s>' % (m.group(0), n, '+0x%x' % off if off else '')
        return m.group(0)

    pat = re.compile(r'0x[0-9a-fA-F]{6,8}')
    with open(sys.argv[1], encoding='utf-8', errors='replace') as f:
        for line in f:
            sys.stdout.write(pat.sub(name, line))


if __name__ == '__main__':
    main()
