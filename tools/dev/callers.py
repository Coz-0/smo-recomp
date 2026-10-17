"""List the functions of a module that call an imported symbol (bl to its PLT stub) or a named function.

    python tools/dev/callers.py <symbol substring> [--module main]

Same inputs as disasm.py (build/functions/<module>_functions.tsv and the converted dump); prints addresses and names only.
SMO_TARGET picks the game version (tools/target.py).
"""
import argparse
import bisect
import csv
import os
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, 'tools', 'extract'))
sys.path.insert(0, os.path.join(REPO, 'tools'))
import functions as F  # noqa: E402
import target as T  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('what')
    ap.add_argument('--module', default='main')
    a = ap.parse_args()
    rows = list(csv.DictReader(open(T.build('functions', '%s_functions.tsv' % a.module), encoding='utf-8'), delimiter='\t'))
    names = {int(r['start'], 16): (r['symbol'] or r['decomp_name']).split('|')[0] for r in rows}
    ends = {int(r['start'], 16): int(r['end'], 16) for r in rows}
    m = F.Module(T.elf(a.module))
    if 23 in m.dt:
        syms = m.symbols()
        slot = {}
        for i in range(m.dt[2] // 24):
            off, info, _ = struct.unpack_from('<QQq', m.d, 0x1000 + m.dt[23] + i * 24)
            slot[off] = syms[info >> 32][0]
        for s in ends:
            w0, w1 = struct.unpack_from('<II', m.d, 0x1000 + s)
            if (w0 & 0x9f00001f) == 0x90000010 and (w1 & 0xffc003ff) == 0xf9400211:
                imm = ((w0 >> 5) & 0x7ffff) << 2 | ((w0 >> 29) & 3)
                if imm & (1 << 20):
                    imm -= 1 << 21
                n = slot.get((s & ~0xfff) + (imm << 12) + ((w1 >> 10) & 0xfff) * 8)
                if n:
                    names[s] = 'PLT:' + n
    targets = {s for s, n in names.items() if a.what in n}
    if not targets:
        raise SystemExit('no function matches %r' % a.what)
    starts = sorted(ends)
    for s in sorted(starts):
        for pc in range(s, ends[s], 4):
            w, = struct.unpack_from('<I', m.d, 0x1000 + pc)
            if (w & 0xfc000000) in (0x94000000, 0x14000000):
                imm = w & 0x3ffffff
                if imm & (1 << 25):
                    imm -= 1 << 26
                t = pc + imm * 4
                if t in targets:
                    print('%x in %x %s -> %s' % (pc, s, names.get(s, '?'), names[t]))


if __name__ == '__main__':
    main()
