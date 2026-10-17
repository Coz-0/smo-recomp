"""Instruction-frequency scan of the listed functions of a module, with the lifter's translation status.

    python tools/lifter/insnfreq.py [--top 60] [--out build/lift/insnfreq.json]

Every 32-bit word inside a listed function (build/functions/main_functions.tsv, exact extents) is classified by
Capstone as `mnemonic shape` (registers and immediates normalised, as difftest.py groups them) and lifted with
recomp.Fn.lift: translated, or untranslated with the lifter's reason. Prints the most frequent shapes and every
untranslated shape; writes the full table as JSON. Nothing of the game is printed except mnemonics and counts.
"""
import argparse
import collections
import json
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import recomp
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

REG_RE = re.compile(r'\b([vqdshbwx])\d+\b')
IMM_RE = re.compile(r'#-?(0x)?[0-9a-f.e+-]+')


def shape_of(md, w, cache):
    s = cache.get(w)
    if s is None:
        ins = list(md.disasm(struct.pack('<I', w), 0x1000000))
        if not ins:
            s = '(undecodable)'
        else:
            s = ins[0].mnemonic + ' ' + REG_RE.sub(r'\1', IMM_RE.sub('#', ins[0].op_str))
        cache[w] = s
    return s


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--top', type=int, default=60)
    ap.add_argument('--out', default=os.path.join(recomp._REPO, 'build', 'lift', 'insnfreq.json'))
    a = ap.parse_args()
    env = recomp.Env()
    env.imports = set()
    env.compute_starts()
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    fn = recomp.Fn(recomp.BASE, recomp.BASE + recomp.TEXT_SIZE, env)
    shape_cache, lift_cache = {}, {}
    count = collections.Counter()
    fails = collections.Counter()
    reason = {}
    mnem = collections.Counter()
    words = 0
    for s in env.starts:
        e = env.ends[s]
        for pc in range(s, e, 4):
            w = recomp.word(pc)
            words += 1
            sh = shape_of(md, w, shape_cache)
            count[sh] += 1
            mnem[sh.split()[0]] += 1
            r = lift_cache.get(w)
            if r is None:
                try:
                    fn.lift(w, recomp.BASE + 0x1000000)
                    r = ''
                except recomp.Unimpl as ex:
                    r = str(ex)
                except Exception as ex:                       # decoder crash: count as untranslated
                    r = 'error:' + type(ex).__name__
                lift_cache[w] = r
            if r:
                fails[sh] += 1
                reason[sh] = r
    bad = sum(fails.values())
    print('functions %d   instruction words %d   distinct shapes %d   distinct mnemonics %d'
          % (len(env.starts), words, len(count), len(mnem)))
    print('translated %d (%.4f%%)   untranslated %d (%.4f%%) in %d shapes'
          % (words - bad, 100.0 * (words - bad) / words, bad, 100.0 * bad / words, len(fails)))
    print('\nmost frequent shapes:')
    for sh, n in count.most_common(a.top):
        print('  %9d  %6.3f%%  %s%s' % (n, 100.0 * n / words, sh, '   [UNTRANSLATED: %s]' % reason[sh] if sh in fails else ''))
    print('\nuntranslated shapes (all):')
    for sh, n in fails.most_common():
        print('  %9d  %-40s %s' % (n, reason[sh], sh))
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    json.dump({'words': words, 'shapes': count.most_common(), 'mnemonics': mnem.most_common(),
               'untranslated': [(sh, n, reason[sh]) for sh, n in fails.most_common()]}, open(a.out, 'w'), indent=0)


if __name__ == '__main__':
    main()
