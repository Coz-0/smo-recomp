"""Disassemble one function of a module with Capstone, naming call / branch targets from the function list (tools/extract/functions.py).

    python tools/dev/disasm.py <symbol substring | hex address> [--module main] [--max 400]

Addresses are module-relative (main loads at 0x7100000000). PLT stubs of main are named by their import (JMPREL).
Reads only the converted dump in build/; prints instructions, not data.
"""
import argparse
import csv
import os
import struct
import sys

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, 'tools', 'extract'))
import functions as F  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('what')
    ap.add_argument('--module', default='main')
    ap.add_argument('--max', type=int, default=400)
    a = ap.parse_args()
    rows = list(csv.DictReader(open(os.path.join(REPO, 'build', 'functions', '%s_functions.tsv' % a.module), encoding='utf-8'), delimiter='\t'))
    names = {int(r['start'], 16): (r['symbol'] or r['decomp_name']).split('|')[0] for r in rows}
    ends = {int(r['start'], 16): int(r['end'], 16) for r in rows}
    m = F.Module(os.path.join(REPO, 'build', 'dump', 'base', 'elf', '%s.elf' % a.module))
    # PLT stubs -> import names
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
                page = (s & ~0xfff) + (imm << 12)
                n = slot.get(page + ((w1 >> 10) & 0xfff) * 8)
                if n:
                    names[s] = 'PLT:' + n
    try:
        start = int(a.what, 16)
        if start >= 0x7100000000:
            start -= 0x7100000000
    except ValueError:
        hits = [s for s, n in names.items() if a.what in n]
        if not hits:
            raise SystemExit('no function matches %r' % a.what)
        start = hits[0]
        if len(hits) > 1:
            print('(%d matches; first shown)' % len(hits))
    end = ends.get(start, start + 4 * a.max)
    print('%x..%x %s' % (start, end, names.get(start, '?')))
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    for k, ins in enumerate(md.disasm(m.d[0x1000 + start:0x1000 + end], start)):
        if k >= a.max:
            break
        note = ''
        if ins.mnemonic in ('bl', 'b') or ins.mnemonic.startswith('b.'):
            try:
                t = int(ins.op_str.split('#')[-1], 16)
                if t in names and not (start <= t < end):
                    note = '   ; ' + names[t][:110]
            except ValueError:
                pass
        print('  %8x  %-7s %s%s' % (ins.address, ins.mnemonic, ins.op_str, note))


if __name__ == '__main__':
    main()
