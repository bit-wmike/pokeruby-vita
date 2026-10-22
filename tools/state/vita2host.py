#!/usr/bin/env python3
"""Convert a Vita savestate (savestate.bin) for the host build of the same
source version, so that a state from Mike's Vita reproduces on the PC.

usage: vita2host.py vita.elf host_binary savestate.bin out.bin [--delta 0xB000]

The game's C globals live in other places on each build: every block is
copied symbol by symbol (same name, n-th occurrence) and every word that
points into the Vita image is translated to the host symbol + offset.
The Vita loader relocates the module (runtime = link address + delta)."""
import bisect, struct, subprocess, sys

def syms(path, nm):
    out = subprocess.run([nm, '-n', '-S', path], capture_output=True, text=True).stdout
    r = []
    for l in out.splitlines():
        p = l.split()
        if len(p) == 4:
            r.append((int(p[0], 16), int(p[1], 16), p[2], p[3]))
        elif len(p) == 3 and p[1] not in 'wUv':
            r.append((int(p[0], 16), 0, p[1], p[2]))
    r.sort()
    return r

def section_bytes(path, name, objcopy):
    import tempfile, os
    t = tempfile.mktemp()
    subprocess.run([objcopy, '-O', 'binary', '-j', name, path, t], check=True)
    d = open(t, 'rb').read(); os.unlink(t)
    return d

def main():
    velf, hbin, sin, sout = sys.argv[1:5]
    # the Vita loader moves the two segments apart: code + read-only data
    # by one delta, writable data (from .data on) by another
    delta = int(sys.argv[sys.argv.index('--delta') + 1], 16) if '--delta' in sys.argv else 0xB000
    ddelta = int(sys.argv[sys.argv.index('--ddelta') + 1], 16) if '--ddelta' in sys.argv else 0xF0000
    V = [x for x in syms(velf, '/usr/local/vitasdk/bin/arm-vita-eabi-nm') if 0x81000000 <= x[0] < 0x83000000 and x[2] not in 'aA']
    hnm = 'arm-linux-gnueabihf-nm' if '--arm' in sys.argv else 'nm'
    hobjcopy = 'arm-linux-gnueabihf-objcopy' if '--arm' in sys.argv else 'objcopy'
    H = syms(hbin, hnm)
    vn = {s[3]: s for s in V}; hn = {s[3]: s for s in H}
    d = open(sin, 'rb').read()
    magic, ver = d[:8], d[8:32]
    nb, total = struct.unpack('<II', d[32:40]); sizes = struct.unpack('<16I', d[40:104])[:nb]
    stack = struct.unpack('<I', d[104:108])[0]
    off = 108; blocks = []
    for s in sizes:
        blocks.append(bytearray(d[off:off + s])); off += s
    raw = [('gHostEwram', 0x40000), ('gHostIwram', 0x8000), ('gHostIo', 0x400), ('gHostPalram', 0x400),
           ('gHostVram', 0x1E000), ('gHostOam', 0x400)]
    secs = ['game_bss', 'game_data', 'ewram_data']
    # occurrence-indexed symbol maps per section
    def occ(S, lo, hi):
        m = {}; c = {}
        for s in S:
            if lo <= s[0] < hi and s[1] > 0:
                k = c.get(s[3], 0); c[s[3]] = k + 1
                m[(s[3], k)] = s
        return m
    out = []
    for i, (n, sz) in enumerate(raw):
        assert len(blocks[i]) == sz, (n, len(blocks[i]))
        out.append(bytearray(blocks[i]))
    bi = len(raw)
    for sec in secs:
        if '__start_' + sec not in vn:
            continue
        vlo, vhi = vn['__start_' + sec][0], vn['__stop_' + sec][0]
        hlo, hhi = hn['__start_' + sec][0], hn['__stop_' + sec][0]
        vb = blocks[bi]; bi += 1
        assert len(vb) == vhi - vlo, (sec, len(vb), vhi - vlo)
        hb = bytearray(hhi - hlo) if sec.endswith('bss') else bytearray(section_bytes(hbin, sec, hobjcopy))
        assert len(hb) == hhi - hlo, (sec, len(hb), hhi - hlo)
        vm, hm = occ(V, vlo, vhi), occ(H, hlo, hhi)
        miss = 0
        for key, vs in vm.items():
            hs = hm.get(key)
            if not hs:
                miss += 1; continue
            n = min(vs[1], hs[1])
            hb[hs[0] - hlo:hs[0] - hlo + n] = vb[vs[0] - vlo:vs[0] - vlo + n]
        print(f'{sec}: {len(vm)} symbols, {miss} without host match', file=sys.stderr)
        out.append(hb)
    for name in ('gUnknown_030007B8',):
        out.append(bytearray(blocks[bi])); bi += 1
    out.append(bytearray(blocks[bi])); bi += 1   # HDMA state
    assert bi == nb
    if '--delta' not in sys.argv:   # find the loader's shifts: most pointers land on symbol starts
        vstarts_d = set(x[0] for x in V if x[2] in 'DdBb' and x[1] > 0)
        vstarts_r = set(x[0] for x in V if x[2] in 'RrTt' and x[1] > 0)
        words = []
        for k in (0, 1, 6, 7, 8):
            b = blocks[k]
            words += [struct.unpack_from('<I', b, i)[0] for i in range(0, len(b) - 3, 4)]
        words = [w for w in words if 0x81000000 <= w < 0x83000000]
        def best(starts, odd):
            c = {}
            for d0 in range(-0x400000, 0x400000, 0x1000):
                c[d0] = sum(1 for w in words if ((w & ~1) if odd else w) - d0 in starts)
            return max(c, key=c.get)
        delta, ddelta = best(vstarts_r, True), best(vstarts_d, False)
        print(f'deltas: code {delta:#x}, data {ddelta:#x}', file=sys.stderr)
    # pointer translation
    vaddr = [s[0] for s in V]
    hsym_occ = {}
    c = {}
    vocc = {}
    for s in V:
        k = c.get(s[3], 0); c[s[3]] = k + 1; vocc[s] = k
    c = {}
    for s in H:
        k = c.get(s[3], 0); c[s[3]] = k + 1; hsym_occ[(s[3], k)] = s
    datalo = vn['__start_game_data'][0] - 0x10000   # .data precedes game_data
    datalo = min(s[0] for s in V if s[2] in 'Dd')
    imglo, imghi = V[0][0] + delta, datalo + delta
    dlo, dhi = datalo + ddelta, V[-1][0] + V[-1][1] + ddelta
    stats = {'data': 0, 'func': 0, 'stack': 0, 'left': 0}
    def T(w):
        if stack - 0x40000 <= w < stack + 0x100000 and not dlo <= w < dhi:
            stats['stack'] += 1; return w
        l = w - ddelta if dlo <= w < dhi else w - delta
        j = bisect.bisect_right(vaddr, l) - 1
        if j < 0:
            return w
        s = V[j]
        h = hsym_occ.get((s[3], vocc[s]))
        if s[2] in 'Tt':
            if (l & ~1) == s[0] and h:
                stats['func'] += 1; return h[0] | (w & 1 if '--arm' in sys.argv else 0)   # Thumb bit
            stats['left'] += 1; return w
        o = l - s[0]
        if h and (o <= s[1] and o <= h[1] or s[1] == 0):   # sized symbol, or an assembler label
            stats['data'] += 1; return h[0] + o
        stats['left'] += 1; return w
    for k, b in enumerate(out):
        if k in (2, 3, 4, 5):   # IO, palettes, VRAM, OAM: no pointers
            continue
        for i in range(0, len(b) - 3, 4):
            w = struct.unpack_from('<I', b, i)[0]
            if imglo <= w < imghi or dlo <= w < dhi or stack - 0x40000 <= w < stack + 0x100000:
                struct.pack_into('<I', b, i, T(w) & 0xFFFFFFFF)
    print('pointers:', stats, file=sys.stderr)
    hver = sys.argv[sys.argv.index('--version') + 1].encode() if '--version' in sys.argv else ver.rstrip(b'\0')
    hs = [len(b) for b in out]
    hdr = magic + hver.ljust(24, b'\0') + struct.pack('<II', len(out), sum(hs)) + struct.pack('<16I', *(hs + [0] * (16 - len(hs)))) + struct.pack('<I', stack)
    open(sout, 'wb').write(hdr + b''.join(out))

main()
