"""Differential test of the lifter against the Unicorn AArch64 emulator (the reference).

Adapted from ssbu-recomp tools/lifter/difftest.py (GPL-3.0-or-later). For every instruction shape found in the
listed functions of SMO's `main`, a few real instruction words are executed twice from the same pseudo-random machine
state: once by Unicorn, once by the C code recomp.py generates. Final registers, flags and memory must be identical.

    python tools/lifter/difftest.py sample   collect sample words from the listed functions -> samples.json
    python tools/lifter/difftest.py gen      generate difftest_gen.c + tests.json from the samples
    python tools/lifter/difftest.py build    compile difftest.exe with clang (LLVM_ROOT, default C:/llvm-9.0.0)
    python tools/lifter/difftest.py check    run difftest.exe and Unicorn, compare; exit code 1 on any failure
    python tools/lifter/difftest.py show <i> registers that differ for one test
    python tools/lifter/difftest.py all      sample + gen + build + check
Working files live in build/difftest/ (build/<version>/difftest/ for an update target: SMO_TARGET, tools/target.py).

Test kinds (column `kind`):
  straight  data processing, loads/stores, FP/SIMD: registers + NZCV + a memory window must match (ssbu-recomp's test)
  special   FP instructions again from states made of NaN / signed zero / subnormal / half-tie operands (ssbu-recomp)
  branch    conditional branches (b.cond, cbz, cbnz, tbz, tbnz): taken / not taken must match Unicorn's PC, registers
            unchanged  [added for SMO]
  trap      words that are not instructions on the console's CPU: Unicorn must fault, the lifted code must call
            guest_trap  [added for SMO]
What is stricter than ssbu-recomp: a sampled word the lifter cannot translate is a FAILURE (ssbu-recomp skipped it).
What is not tested here, by design (reported per shape): unconditional control transfer (b, bl, blr, br, ret: they
leave the function and are exercised by the program), svc / system registers / barriers, and exclusive load/store
(the single-threaded reference cannot model the monitor).
"""
import collections
import json
import os
import re
import struct
import subprocess
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import recomp

T = recomp._T                    # SMO_TARGET (tools/target.py): which package's modules, which build/
WORK = T.build('difftest')
HERE = os.path.dirname(os.path.abspath(__file__))
RUNTIME = os.path.join(recomp._REPO, 'runtime')
LLVM = os.environ.get('LLVM_ROOT', 'C:/llvm-9.0.0')
HOST_EXE = os.environ.get('DIFFTEST_EXE') or os.path.join(WORK, 'difftest.exe')

REGION, REGION_SIZE = 0x7200000000, 0x400000
CODE, DATA = REGION + 0x200000, REGION + 0x300000
WIN_LO, WIN_HI = DATA - 0x1000, DATA + 0x12000
M64 = (1 << 64) - 1
PER_SHAPE = 3
COND_BRANCH = ('b.', 'cbz', 'cbnz', 'tbz', 'tbnz')


class Rng:
    def __init__(self, seed):
        self.s = (seed * 0x9E3779B97F4A7C15 + 0x1234567) & M64

    def next(self):
        self.s = (self.s + 0x9E3779B97F4A7C15) & M64
        z = self.s
        z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & M64
        z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & M64
        return z ^ (z >> 31)


def pattern():
    r = Rng(0xABCDEF)
    return b''.join(struct.pack('<Q', r.next()) for _ in range(REGION_SIZE // 8))


def initial_state(idx, mem, rn, rm):
    r = Rng(idx)
    x = [r.next() for _ in range(31)] + [DATA + 0x800]
    v = []
    for _ in range(32):
        lanes = [(r.next() % 200001 - 100000) / 64.0 for _ in range(4)]
        v.append(struct.pack('<4f', *lanes))
    flags = r.next() & 0xf
    if mem:
        base = DATA + 0x100 * (r.next() % 8)
        index = r.next() % 0x40
        if rn != 31:
            x[rn] = base
        if 0 <= rm < 31 and rm != rn:
            x[rm] = index
    return x, v, flags


# ---- floating-point special operands (ssbu-recomp, unchanged) ------------------------------------------------------
SPECIAL = {32: [0x00000000, 0x80000000, 0x3f800000, 0xbf800000, 0x40200000, 0xc0200000, 0x7f800000, 0xff800000, 0x7fc00000, 0xffc00000, 0x7fc12345, 0x7f800001, 0xffa00000, 0x00000001, 0x007fffff, 0x80000400, 0x00800000, 0x7f7fffff, 0x477fe000, 0x477ff000, 0x477fefff, 0x33800000, 0x33000000, 0x33000001, 0x38800000, 0x387fffff, 0x3f801000, 0x3f803000, 0x3f801001],
           64: [0x0000000000000000, 0x8000000000000000, 0x3ff0000000000000, 0xbff0000000000000, 0x4004000000000000, 0xc004000000000000, 0x7ff0000000000000, 0xfff0000000000000, 0x7ff8000000000000, 0xfff8000000000000, 0x7ff8000000012345, 0x7ff0000000000001, 0xfff4000000000000, 0x0000000000000001, 0x000fffffffffffff, 0x8000000000000400, 0x0010000000000000, 0x7fefffffffffffff, 0x40effc0000000000, 0x40effe0000000000, 0x40effdffffffffff, 0x3e70000000000000, 0x3e60000000000000, 0x3e60000000000001, 0x3f10000000000000, 0x3f0fffffffffffff, 0x3ff0020000000000, 0x3ff0060000000000, 0x3ff0020000001000],
           16: [0x0000, 0x8000, 0x3c00, 0xbc00, 0x7c00, 0xfc00, 0x7e00, 0xfe00, 0x7e01, 0x7c01, 0xfd55, 0x0001, 0x03ff, 0x8200, 0x0400, 0x7bff, 0x3555, 0xc123]}
FP_SPECIAL_MNEMONICS = ('fmax', 'fmin', 'fmaxnm', 'fminnm', 'fcvt')
if os.environ.get('DIFFTEST_FP_ALL'):
    FP_SPECIAL_MNEMONICS += ('fadd', 'fsub', 'fmul', 'fdiv', 'fnmul', 'fmadd', 'fmsub', 'fnmadd', 'fnmsub', 'fsqrt', 'fabd', 'faddp', 'fmulx', 'fmla', 'fmls',
                             'frinta', 'frinti', 'frintm', 'frintn', 'frintp', 'frintx', 'frintz', 'fmaxp', 'fminp', 'fmaxnmp', 'fminnmp', 'frecpe', 'frsqrte', 'fneg', 'fabs')
FP_EXTRA_WORDS = [0x1e63c020, 0x1ee24020, 0x1ee2c020, 0x1e624820, 0x1e627820, 0x1e625820, 0x1e626820,
                  0x4e22c420, 0x4ea2c420, 0x4e62f420, 0x4ee2f420, 0x4e62c420, 0x4ee2c420, 0x0e22f420, 0x0ea2f420]


def fp_mode(w):
    if recomp.bits(w, 28, 24) == 0x1e:
        return {0: 32, 1: 64, 3: 16}.get(recomp.bits(w, 23, 22), 0)
    return 64 if recomp.bits(w, 22, 22) else 32


def special_v(t):
    mode, k = t['fpmode'], t['k']
    table = SPECIAL[mode]
    n = len(table)
    ia, ib = k % n, (k // n) % n
    v = []
    for r in range(32):
        b = ib if r == t['frm'] else ia if r == t['rn'] else (k + r) % n
        lanes = [table[(b + 7 * l) % n] for l in range(128 // mode)]
        v.append(b''.join(x.to_bytes(mode // 8, 'little') for x in lanes))
    return v


def _md():
    from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
    return Cs(CS_ARCH_ARM64, CS_MODE_ARM)


def shape_key(md, w):
    ins = list(md.disasm(struct.pack('<I', w), CODE))
    if not ins:
        return '(undecodable)'
    shape = re.sub(r'#-?(0x)?[0-9a-f.e+-]+', '#', ins[0].op_str)
    shape = re.sub(r'\b([vqdshbwx])\d+\b', r'\1', shape)
    return ins[0].mnemonic + ' ' + shape


def build_special(fn, old):
    md = _md()
    words = [(t['key'], t['word']) for t in old if t['key'].split()[0] in FP_SPECIAL_MNEMONICS]
    for w in FP_EXTRA_WORDS:
        ins = list(md.disasm(struct.pack('<I', w), CODE))
        words.append(('(not in the game) %s %s' % (ins[0].mnemonic, ins[0].op_str), w))
    tests = []
    for key, w in words:
        try:
            stmt = fn.lift(w, CODE)
        except recomp.Unimpl:
            stmt = 'guest_unimpl(0, 0, "untranslated");'
        mode = fp_mode(w)
        if mode not in SPECIAL:
            continue
        unary = key.split()[0] == 'fcvt' or 'fcvt' in key
        n = len(SPECIAL[mode])
        for k in range(n if unary else n * n):
            tests.append({'kind': 'special', 'key': key + ' [special]', 'word': w, 'stmt': stmt, 'mem': 0,
                          'rn': recomp.bits(w, 9, 5), 'rm': -1, 'fpmode': mode, 'k': k,
                          'frm': -1 if unary else recomp.bits(w, 20, 16), 'target': 0})
    return tests


# ---------------------------------------------------------------------------------------------------------- commands
def module_words(spec):
    """Every instruction word inside the listed functions of a module of the dump. spec "sdk:lifted" keeps only the
    functions sdklift.py lifted (the SDK code that runs); plain "sdk" would take all of them."""
    module, _, only = spec.partition(':')
    keep = None
    if only == 'lifted':
        gen = T.build('gen', 'sdklift_gen.c')
        keep = set(int(m, 16) - 0x7500000000 for m in re.findall(r'^\s+\{0x([0-9a-f]+)ULL, f_', open(gen).read(), re.M)) if os.path.exists(gen) else set()
    elf = open(T.elf(module), 'rb').read()
    with open(T.build('functions', '%s_functions.tsv' % module), encoding='utf-8') as f:
        next(f)
        for line in f:
            t = line.split('\t')
            s, e = int(t[0], 16), int(t[1], 16)
            if keep is not None and s not in keep:
                continue
            for a in range(s, e, 4):
                yield struct.unpack_from('<I', elf, 0x1000 + a)[0]


def cmd_sample():
    """Shapes of the listed functions of every module in DIFFTEST_MODULES (default main and sdk:lifted, the SDK functions sdklift.py lifted
    too, by sdklift.py). A shape keeps up to PER_SHAPE words, taken from main first."""
    md = _md()
    seen = set()
    shapes = collections.defaultdict(list)
    per_module = collections.Counter()
    for module in os.environ.get('DIFFTEST_MODULES', 'main,sdk:lifted').split(','):
        for w in module_words(module):
            if w in seen:
                continue
            seen.add(w)
            key = shape_key(md, w)
            if key not in shapes:
                per_module[module] += 1
            if len(shapes[key]) < PER_SHAPE:
                shapes[key].append(w)
    json.dump(shapes, open('samples.json', 'w'))
    print('shapes: %d (%s)   sample words: %d   distinct words: %d'
          % (len(shapes), ', '.join('first seen in %s: %d' % kv for kv in per_module.items()),
             sum(len(v) for v in shapes.values()), len(seen)))


def classify(fn, w, key):
    """(kind or None, stmt or reason). kind None = not tested here, reason given."""
    mnem = key.split()[0]
    try:
        stmt = fn.lift(w, CODE)
    except recomp.Unimpl as e:
        return 'untranslated', str(e)
    if stmt.startswith('guest_trap("undefined instruction"'):
        return 'trap', stmt
    op0 = recomp.bits(w, 28, 25)
    if op0 in (10, 11):
        if mnem.startswith(COND_BRANCH):
            return 'branch', stmt
        if mnem in ('b', 'bl', 'blr', 'br', 'ret'):
            return None, 'control transfer out of the function'
        if stmt.strip() == ';':
            return 'straight', stmt                       # hint / barrier / clrex: architectural state unchanged
        return None, 'svc / system register access'
    if op0 in (4, 6, 12, 14) and recomp.bits(w, 29, 24) == 8 and not recomp.bits(w, 26, 26):
        if not recomp.bits(w, 23, 23):                    # o2 = 0: exclusive (LDXR / STXR family)
            return None, 'exclusive access (the single-threaded reference cannot model the monitor)'
        # o2 = 1: LDAR / STLR, ordered but not exclusive: an ordinary load/store for a single thread -> tested
    if any(t in stmt for t in ('goto', 'return', 'call_indirect', 'guest_', 'f_7')):
        return None, 'leaves straight-line code'
    return 'straight', stmt


def build_tests():
    shapes = json.load(open('samples.json'))
    env = recomp.Env()
    env.imports = set()
    # a wide function around CODE, so every conditional-branch target becomes a local label (goto L_<target>)
    fn = recomp.Fn(CODE - 0x200000, CODE + 0x200000, env)
    tests, untranslated, excluded = [], [], collections.defaultdict(list)
    shape_kind = {}
    for key, words in sorted(shapes.items()):
        kinds = set()
        for w in words:
            kind, stmt = classify(fn, w, key)
            if kind == 'untranslated':
                untranslated.append((key, w, stmt))
                kinds.add('UNTRANSLATED')
                continue
            if kind is None:
                excluded[stmt].append(key)
                kinds.add('excluded: ' + stmt)
                continue
            kinds.add(kind)
            op0 = recomp.bits(w, 28, 25)
            mem = int(kind == 'straight' and op0 in (4, 6, 12, 14))
            rn = recomp.bits(w, 9, 5)
            rm = -1
            if mem and recomp.bits(w, 29, 27) == 7 and recomp.bits(w, 24, 24) == 0 and recomp.bits(w, 21, 21) \
                    and recomp.bits(w, 11, 10) == 2:
                rm = recomp.bits(w, 20, 16)
            target = 0
            if kind == 'branch':
                m = re.search(r'goto L_([0-9a-f]+);', stmt)
                target = int(m.group(1), 16)
            tests.append({'kind': kind, 'key': key, 'word': w, 'stmt': stmt, 'mem': mem, 'rn': rn, 'rm': rm,
                          'fpmode': 0, 'k': 0, 'frm': -1, 'target': target})
        shape_kind[key] = sorted(kinds)
    if not os.environ.get('DIFFTEST_NO_SPECIAL'):
        tests += build_special(fn, [t for t in tests if t['kind'] == 'straight'])
    return tests, untranslated, excluded, shape_kind


def cmd_gen():
    tests, untranslated, excluded, shape_kind = build_tests()
    with open('difftest_gen.c', 'w') as f:
        f.write('/* generated by difftest.py -- do not edit */\n#include "runtime.h"\n\nextern volatile int dt_event;\n\n')
        for i, t in enumerate(tests):
            if t['kind'] == 'branch':
                f.write('static void t_%d(Ctx *c) { %s dt_event = 0; return; L_%x: dt_event = 1; }\n'
                        % (i, t['stmt'], t['target']))
            else:
                f.write('static void t_%d(Ctx *c) { %s }\n' % (i, t['stmt']))
        f.write('\ntypedef struct { void (*fn)(Ctx *); uint32_t word; int mem, rn, rm, fpmode, k, frm; } Test;\n')
        f.write('const Test tests[] = {\n')
        for i, t in enumerate(tests):
            f.write('  { t_%d, 0x%08xU, %d, %d, %d, %d, %d, %d },\n'
                    % (i, t['word'], t['mem'], t['rn'], t['rm'], t['fpmode'], t['k'], t['frm']))
        f.write('};\nconst int test_count = %d;\n' % len(tests))
    json.dump(tests, open('tests.json', 'w'))
    json.dump({'untranslated': untranslated, 'excluded': excluded, 'shape_kind': shape_kind},
              open('coverage.json', 'w'), indent=0)
    kinds = collections.Counter(t['kind'] for t in tests)
    print('tests generated: %d  (%s)' % (len(tests), ', '.join('%s %d' % kv for kv in sorted(kinds.items()))))
    tested = sum(1 for k in shape_kind.values() if any(x in ('straight', 'branch', 'trap') for x in k))
    print('shapes: %d   tested: %d   excluded by design: %d   untranslated: %d'
          % (len(shape_kind), tested, sum(1 for k in shape_kind.values() if all(x.startswith('excluded') for x in k)),
             sum(1 for k in shape_kind.values() if 'UNTRANSLATED' in k)))
    for reason, keys in sorted(excluded.items()):
        print('  excluded (%s): %d shapes, e.g. %s' % (reason, len(set(keys)), ', '.join(sorted(set(keys))[:6])))
    for key, w, why in untranslated:
        print('  UNTRANSLATED %08x %s (%s)' % (w, key, why))


def cmd_build():
    clang = os.path.join(LLVM, 'bin', 'clang.exe')
    host = os.path.join(HERE, 'difftest_host.c')
    cmd = [clang, '-O0', '-w', '-fno-strict-aliasing', '-I', RUNTIME, 'difftest_gen.c', host, '-o', HOST_EXE]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        print(r.stdout[-3000:], r.stderr[-3000:])
        sys.exit(1)
    print('built', HOST_EXE)


def run_reference(uc, A, t, i, xr, vr, window):
    from unicorn import UcError
    x, v, flags = initial_state(i, t['mem'], t['rn'], t['rm'])
    if t.get('fpmode'):
        v = special_v(t)
    for r, val in zip(xr, x[:31]):
        uc.reg_write(r, val)
    uc.reg_write(A.UC_ARM64_REG_SP, x[31])
    for r, val in zip(vr, v):
        uc.reg_write(r, int.from_bytes(val, 'little'))
    uc.reg_write(A.UC_ARM64_REG_NZCV, flags << 28)
    uc.mem_write(WIN_LO, window)
    uc.mem_write(CODE, struct.pack('<I', t['word']))
    uc.reg_write(A.UC_ARM64_REG_PC, CODE)
    try:
        uc.emu_start(CODE, CODE + 4, count=1)
    except UcError as e:
        return None, 2, str(e)
    regs = b''.join(struct.pack('<Q', uc.reg_read(r)) for r in xr) + struct.pack('<Q', uc.reg_read(A.UC_ARM64_REG_SP))
    regs += b''.join(uc.reg_read(r).to_bytes(16, 'little') for r in vr)
    regs += struct.pack('<I', (uc.reg_read(A.UC_ARM64_REG_NZCV) >> 28) & 0xf)
    pc = uc.reg_read(A.UC_ARM64_REG_PC)
    event = 0
    if t['kind'] == 'branch':
        event = 1 if pc == t['target'] else 0 if pc == CODE + 4 else -1
    return (zlib.crc32(regs), zlib.crc32(bytes(uc.mem_read(WIN_LO, WIN_HI - WIN_LO)))), event, None


def make_uc():
    from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
    from unicorn import arm64_const as A
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.mem_map(REGION, REGION_SIZE)
    uc.mem_write(REGION, pattern())
    uc.reg_write(A.UC_ARM64_REG_CPACR_EL1, 0x300000)
    xr = [getattr(A, 'UC_ARM64_REG_X%d' % i) for i in range(29)] + [A.UC_ARM64_REG_X29, A.UC_ARM64_REG_X30]
    vr = [getattr(A, 'UC_ARM64_REG_V%d' % i) for i in range(32)]
    return uc, A, xr, vr


def cmd_check():
    tests = json.load(open('tests.json'))
    cov = json.load(open('coverage.json'))
    out = subprocess.run([HOST_EXE], capture_output=True, text=True).stdout.split('\n')
    host = {}
    for line in out:
        p = line.split()
        if len(p) == 4:
            host[int(p[0])] = ((int(p[1], 16), int(p[2], 16)), int(p[3]))
    uc, A, xr, vr = make_uc()
    window = pattern()[WIN_LO - REGION:WIN_HI - REGION]
    bad = collections.defaultdict(list)
    ok = collections.Counter()
    total = collections.Counter()
    for i, t in enumerate(tests):
        total[t['kind']] += 1
        ref, ev, err = run_reference(uc, A, t, i, xr, vr, window)
        h = host.get(i)
        if t['kind'] == 'trap':
            good = err is not None and h is not None and h[1] == 2
        elif err is not None:
            bad['(reference fault: %s) %s' % (err, t['key'])].append(i)
            continue
        elif t['kind'] == 'branch':
            good = h is not None and h == (ref, ev) and ev in (0, 1)
        else:
            good = h is not None and h[0] == ref and h[1] == 0
        if good:
            ok[t['kind']] += 1
        else:
            bad[t['key']].append(i)
    n_untr = len(cov['untranslated'])
    failing = sum(total.values()) - sum(ok.values())
    print('tests: %d   identical: %d   different: %d   untranslated sample words: %d'
          % (sum(total.values()), sum(ok.values()), failing, n_untr))
    for k in sorted(total):
        print('  %-9s %6d identical of %6d' % (k, ok[k], total[k]))
    for key, idxs in sorted(bad.items(), key=lambda kv: -len(kv[1]))[:60]:
        t = tests[idxs[0]]
        print('  DIFF %-52s %d   e.g. #%d %08x' % (key[:52], len(idxs), idxs[0], t['word']))
    json.dump({k: v for k, v in bad.items()}, open('bad.json', 'w'))
    sys.exit(1 if failing or n_untr else 0)


def cmd_show(idx):
    tests = json.load(open('tests.json'))
    t = tests[idx]
    print('#%d %s %08x %s\n  %s' % (idx, t['kind'], t['word'], t['key'], t['stmt']))
    uc, A, xr, vr = make_uc()
    x, v, flags = initial_state(idx, t['mem'], t['rn'], t['rm'])
    if t.get('fpmode'):
        v = special_v(t)
    run_reference(uc, A, t, idx, xr, vr, pattern()[WIN_LO - REGION:WIN_HI - REGION])
    ref = {('x%d' % i): uc.reg_read(r) for i, r in enumerate(xr)}
    ref['sp'] = uc.reg_read(A.UC_ARM64_REG_SP)
    ref.update({('v%d' % i): uc.reg_read(r) for i, r in enumerate(vr)})
    ref['nzcv'] = (uc.reg_read(A.UC_ARM64_REG_NZCV) >> 28) & 0xf
    ref['pc'] = uc.reg_read(A.UC_ARM64_REG_PC)
    host = {}
    for line in subprocess.run([HOST_EXE, str(idx)], capture_output=True, text=True).stdout.split('\n'):
        p = line.split()
        if len(p) == 2:
            host[p[0]] = int(p[1], 16)
    init = {('x%d' % i): x[i] for i in range(31)}
    init['sp'] = x[31]
    init.update({('v%d' % i): int.from_bytes(v[i], 'little') for i in range(32)})
    init['nzcv'] = flags
    print('  reference pc %x (target %x); host event %s' % (ref['pc'], t['target'], host.get('event')))
    for k in ref:
        if k != 'pc' and ref[k] != host.get(k):
            print('  %-5s init %x\n        ref  %x\n        host %x' % (k, init.get(k, -1), ref[k], host.get(k, -1)))


if __name__ == '__main__':
    os.makedirs(WORK, exist_ok=True)
    os.chdir(WORK)
    cmd = sys.argv[1]
    if cmd == 'all':
        cmd_sample(); cmd_gen(); cmd_build(); cmd_check()
    elif cmd == 'sample':
        cmd_sample()
    elif cmd == 'gen':
        cmd_gen()
    elif cmd == 'build':
        cmd_build()
    elif cmd == 'check':
        cmd_check()
    else:
        cmd_show(int(sys.argv[2]))
