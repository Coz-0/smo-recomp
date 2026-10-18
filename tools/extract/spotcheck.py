"""Spot-check of function boundaries of build/functions/<module>_functions.tsv with Capstone.

    python tools/extract/spotcheck.py [--n 300] [--seed 1] [--show 3]

For a fixed-seed random sample of functions per module (plus EVERY function whose end is not exact, i.e. not from
.eh_frame / a sized symbol), disassemble [start, end) and check:
  decode     every word in the body decodes as an instruction
  start      the word before `start` is a terminator, padding, or the end of the previous function (no fall-in)
  last       the last instruction is a terminator (ret / b / br / brk / udf), a call to a function that does not return
             (judged: it is the end of the body), or the body falls through into the next listed function
  branches   every direct branch (b, b.cond, cbz, cbnz, tbz, tbnz) lands inside the same function or on the START of a
             listed function (a tail call); a branch into the middle of another function is a boundary error
Prints counts per check and module, and a few disassemblies. Nothing is written.
"""
import argparse
import bisect
import csv
import os
import random
import struct
import sys

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, 'tools'))
import target as T  # noqa: E402
OFF = 0x1000
TERM = {'ret', 'b', 'br', 'brk', 'udf', 'hlt', 'eret', 'braa', 'brab', 'retaa', 'retab'}
COND_BR = ('b.', 'cbz', 'cbnz', 'tbz', 'tbnz')


def load(module):
    rows = list(csv.DictReader(open(T.build('functions', '%s_functions.tsv' % module), encoding='utf-8'), delimiter='\t'))
    img = open(T.elf(module), 'rb').read()
    return rows, img


def check(module, n, seed, show):
    rows, img = load(module)
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    starts = [int(r['start'], 16) for r in rows]
    ends = {int(r['start'], 16): int(r['end'], 16) for r in rows}
    sset = set(starts)
    rng = random.Random(seed)
    sample = rng.sample(range(len(rows)), min(n, len(rows)))
    sample = sorted(set(sample) | {i for i, r in enumerate(rows) if r['end_from'] != 'exact'})
    res = {'checked': 0, 'decode_fail': 0, 'start_fallin': 0, 'last_ok': 0, 'last_fallthrough': 0, 'last_other': 0,
           'branch_into_other_middle': 0, 'tail_branches': 0}
    problems = []
    shown = 0
    for i in sample:
        s, e = int(rows[i]['start'], 16), int(rows[i]['end'], 16)
        if e <= s:
            continue
        res['checked'] += 1
        code = img[OFF + s:OFF + e]
        insns = list(md.disasm(code, s))
        if sum(x.size for x in insns) != e - s:
            res['decode_fail'] += 1
            problems.append('%s %x: decodes %d of %d bytes' % (module, s, sum(x.size for x in insns), e - s))
            continue
        # word before start
        if s >= 4 and (s - 4) not in sset:
            prev = list(md.disasm(img[OFF + s - 4:OFF + s], s - 4))
            pw = struct.unpack_from('<I', img, OFF + s - 4)[0]
            pi = bisect.bisect_right(starts, s - 1) - 1
            prev_end = ends[starts[pi]] if pi >= 0 else 0
            ok = (prev_end <= s and (not prev or prev[0].mnemonic in TERM or prev[0].mnemonic == 'nop' or pw == 0
                                     or prev[0].mnemonic == 'bl'))
            if not ok:
                res['start_fallin'] += 1
                problems.append('%s %x: previous word %s falls into the start' % (
                    module, s, (prev[0].mnemonic + ' ' + prev[0].op_str) if prev else '%08x' % pw))
        last = insns[-1]
        if last.mnemonic in TERM:
            res['last_ok'] += 1
        elif e in sset:
            res['last_fallthrough'] += 1
        else:
            res['last_other'] += 1
            problems.append('%s %x: last instruction %s %s, next listed start is not %x' % (
                module, s, last.mnemonic, last.op_str, e))
        for x in insns:
            if x.mnemonic == 'b' or x.mnemonic.startswith(COND_BR):
                t = int(x.op_str.split('#')[-1], 16)
                if s <= t < e:
                    continue
                if t in sset:
                    res['tail_branches'] += 1
                    continue
                res['branch_into_other_middle'] += 1
                problems.append('%s %x: %x %s %s lands at %x, inside another function' % (
                    module, s, x.address, x.mnemonic, x.op_str, t))
        if shown < show:
            shown += 1
            print('--- %s %x..%x  src %s  %s' % (module, s, e, rows[i]['src'],
                                                 (rows[i]['symbol'] or rows[i]['rtti_name'])[:90]))
            body = insns if len(insns) <= 12 else insns[:5] + [None] + insns[-5:]
            for x in body:
                print('    ...' if x is None else '    %08x  %-8s %s' % (x.address, x.mnemonic, x.op_str))
    return res, problems


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--n', type=int, default=300)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--show', type=int, default=3)
    ap.add_argument('--modules', default='main,sdk,subsdk0,rtld')
    a = ap.parse_args()
    for m in a.modules.split(','):
        res, problems = check(m, a.n, a.seed, a.show)
        print('%-8s %s' % (m, ' '.join('%s=%d' % kv for kv in res.items())))
        for p in problems[:15]:
            print('    ' + p)
        if len(problems) > 15:
            print('    ... %d more' % (len(problems) - 15))


if __name__ == '__main__':
    main()
