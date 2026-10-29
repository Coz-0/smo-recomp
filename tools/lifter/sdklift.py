"""Lift selected functions of the game's own SDK module (build/dump/update/elf/sdk.elf, NintendoSDK 7.3.2) to C.

Adapted from ssbu-recomp tools/lifter/sdklift.py (GPL-3.0-or-later). The instruction translation is recomp.py's (the same
class Fn the main lift and the Unicorn differential test use); this file decides WHICH functions are lifted and how a
call out of lifted code is bound:
  * another SDK function of the closure          -> lifted too, called directly
  * an exported SDK function the GAME imports    -> the HOST handler of that name ("cut"): nn::os, malloc, ...
    (unless the rules say `lift <name>`)
  * a name under `trap`, or a function that does not return (abort, assertion failure, terminate, throw helpers)
                                                 -> a trap that names it
Roots are computed when this runs, not stored: every function the game imports from the SDK whose name matches a
`family` rule and that sdk.elf defines, plus `root <name>` lines. Rules: tools/lifter/sdklift_rules.txt.

    python tools/lifter/sdklift.py gen        -> build/gen/sdklift_gen.c, sdklift_install.h, build/gen/sdklift_report.txt
    python tools/lifter/sdklift.py closure <name substring>   size of one root's closure (nothing written)
"""
import bisect
import collections
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, 'tools'))
import target as T                                   # noqa: E402
SDK_BASE = 0x7500000000
os.environ['RECOMP_ELF'] = T.elf('sdk')
os.environ['RECOMP_FUNCS'] = T.build('functions', 'sdk_functions.tsv')
os.environ['RECOMP_BASE'] = hex(SDK_BASE)
os.environ['RECOMP_ICACHE'] = '0'                   # an indirect call here goes to sdk_dispatch; icache.h caches main's functions
sys.path.insert(0, HERE)
import recomp as R                                   # noqa: E402  (reads the three variables above at import)

sys.path.insert(0, os.path.join(REPO, 'tools', 'extract'))
import functions as F                                # noqa: E402

GEN = T.build('gen')
MOD = F.Module(R.ELF)
NSYM = MOD.sec['.dynsym'][1] // 24

syms, by_addr, obj_syms = {}, collections.defaultdict(list), {}
for name, typ, bind, shndx, value, size in MOD.symbols():
    if shndx and name:
        syms[name] = (value, size, typ)
        if typ == 2:
            by_addr[SDK_BASE + value].append(name)
        elif typ == 1 and size:
            obj_syms[name] = (value, size, typ)

# data slots holding a function address after relocation (GOT entries, method tables): slot -> target
CODE_SLOTS = {}
for off, info, addend in MOD.relocs():
    typ, sym = info & 0xffffffff, info >> 32
    if typ == 0x403 and 0 <= addend < R.TEXT_SIZE and addend % 4 == 0:
        CODE_SLOTS[SDK_BASE + off] = SDK_BASE + addend
    elif typ in (0x101, 0x401):                       # ABS64 / GLOB_DAT to a function the sdk defines (exported methods in vtables)
        _o = 0x1000 + MOD.dt[6] + sym * 24
        _info, _shndx, _value = MOD.d[_o + 4], struct.unpack_from('<H', MOD.d, _o + 6)[0], struct.unpack_from('<Q', MOD.d, _o + 8)[0]
        if _shndx and (_info & 0xf) == 2:
            CODE_SLOTS[SDK_BASE + off] = SDK_BASE + _value + (addend if typ == 0x101 else 0)

GAME_IMPORTS = set(l.strip() for l in open(os.path.join(GEN, 'imports_all.txt')) if l.strip())
THROW_HELPER = re.compile(r'^_ZNKSt3__12[01]__(basic_string|vector_base)_commonILb1EE20__throw_(length_error|out_of_range)Ev$')
NORETURN = re.compile(r'Abort|UnexpectedDefault|OnAssertionFailure|__assert_fail|^abort$|__cxa_throw|__cxa_pure_virtual|_Unwind_Resume|__throw_|terminate|^exit$|^_Exit$')


class Rules:
    def __init__(self):
        self.families, self.rootall, self.roots, self.cut, self.trap, self.lift = [], [], [], set(), set(), set()
        self.ctors, self.ctorsdata, self.datafuncs, self.wrap = [], [], [], set()
        p = os.path.join(HERE, 'sdklift_rules.txt')
        for line in open(p):
            line = line.split('#')[0].strip()
            if not line:
                continue
            kind, arg = line.split(None, 1)
            if kind == 'family':
                self.families.append(re.compile(arg))
            elif kind == 'rootall':
                self.rootall.append(re.compile(arg))
            elif kind == 'ctors':
                self.ctors.append(re.compile(arg))
            elif kind == 'ctorsdata':
                self.ctorsdata.append(re.compile(arg))
            elif kind == 'datafuncs':
                self.datafuncs.append(re.compile(arg))
            elif kind == 'root':
                self.roots.append(arg)
            elif kind == 'wrap':
                self.wrap.add(arg)
            elif kind in ('cut', 'trap', 'lift'):
                getattr(self, kind).add(arg)
            else:
                raise SystemExit('sdklift_rules.txt: unknown rule %r' % kind)

    def data_roots(self):
        """Code addresses stored (RELATIVE relocations) inside the sdk's own DATA objects whose symbol matches a
        `datafuncs` rule: method tables (_ZTV...) and data that holds function pointers (SMO: nn::audio::common::
        SupportTags::* hold the revision function CheckFeatureSupported calls)."""
        if not self.datafuncs:
            return []
        objs = sorted((v, s) for n, (v, s, t) in obj_syms.items() if any(r.search(n) for r in self.datafuncs))
        starts = [o[0] for o in objs]
        out = set()
        for slot, target in CODE_SLOTS.items():
            off = slot - SDK_BASE
            i = bisect.bisect_right(starts, off) - 1
            if i >= 0 and off < objs[i][0] + objs[i][1]:
                out.add(target)
        return sorted(out)

    def root_names(self):
        out = [n for n in sorted(GAME_IMPORTS) if any(f.search(n) for f in self.families) and n in syms and syms[n][2] == 2]
        out += [n for n in self.roots if n in syms and n not in out]
        have = set(out)
        out += sorted(n for n, (v, s, t) in syms.items() if t == 2 and n not in have and any(r.search(n) for r in self.rootall))
        return [n for n in out if n not in self.cut]   # a cut name is answered by the host, never lifted


class Env(R.Env):
    def __init__(self, rules, roots):
        R.Env.__init__(self)
        self.rules = rules
        self.imports = set()
        self.host_names, self.host_index = [], {}
        self.root_set = set(roots)
        self.noreturn_stmt = False

    def host(self, name):
        if name not in self.host_index:
            self.host_index[name] = len(self.host_names)
            self.host_names.append(name)
        return 'sdk_host_call(c, %d);' % self.host_index[name]

    def resolve(self, target):
        """Follow PLT stubs. Returns (address, None) or (None, name of a symbol sdk.elf does not define)."""
        for _ in range(4):
            plt = self.plt_entry(target)
            if not plt:
                return target, None
            sym, shndx, value = plt
            if not shndx:
                return None, self.plt_symbol(target)
            target = SDK_BASE + value
        return target, None

    def call(self, target):
        self.noreturn_stmt = False
        addr, imp = self.resolve(target)
        if imp is not None:                          # malloc & co: defined by the game (main), reached through the host
            return self.host(imp)
        names = by_addr.get(addr, [])
        if 'setjmp' in names or '_setjmp' in names:
            return 'SDK_SETJMP(c);'
        if 'longjmp' in names or '_longjmp' in names:
            self.noreturn_stmt = True
            return '{ sdk_longjmp(c); return; }'
        for n in names:
            if THROW_HELPER.search(n) and n in GAME_IMPORTS and n not in self.rules.trap:
                self.noreturn_stmt = True
                return '{ %s return; }' % self.host(n)
        for n in names:
            if NORETURN.search(n) or n in self.rules.trap:
                self.noreturn_stmt = True
                return '{ sdk_trap_call(c, "%s"); return; }' % n
        for n in names:                              # an explicit `cut` / `wrap` rule wins, even over a root
            if n in self.rules.cut or n in self.rules.wrap:
                if n in self.rules.wrap:
                    self.wanted.setdefault(addr, None)   # still lifted: the host wrapper calls it (sdk_lifted)
                return self.host(n)
        if addr not in self.root_set:
            for n in names:
                if (n in GAME_IMPORTS or n in self.rules.cut) and n not in self.rules.lift:
                    return self.host(n)
        self.wanted.setdefault(addr, None)
        return 'f_%x(c);' % addr


TAILS = ('return;', 'goto', '{ jt', '{ uint64_t jt', '{ f_', '{ sdk_host_call', '{ sdk_trap_call', '{ sdk_longjmp', 'guest_trap')


CODE_SLOT_KEYS = None
TABLE_WINDOW = 0           # 0x400 was tried: 2,847 functions lifted and 0x17af30 still missing


def table_code_pointers(data_addr):
    """Code pointers stored (RELATIVE relocations) in [data_addr, data_addr + TABLE_WINDOW): a table of function
    pointers whose address the code materialises and reads later (SMO: nn::audio::common::CheckFeatureSupported gets
    its function argument this way). Over-including only lifts functions that may never run."""
    global CODE_SLOT_KEYS
    if CODE_SLOT_KEYS is None:
        CODE_SLOT_KEYS = sorted(CODE_SLOTS)
    i = bisect.bisect_left(CODE_SLOT_KEYS, data_addr)
    out = []
    while i < len(CODE_SLOT_KEYS) and CODE_SLOT_KEYS[i] < data_addr + TABLE_WINDOW:
        out.append(CODE_SLOTS[CODE_SLOT_KEYS[i]])
        i += 1
    return out


def scan_code_refs(entry, end, starts_set):
    """Function starts whose address the code [entry, end) materialises (ADRP+ADD, ADR, ADRP+LDR from a code slot), and
    the code pointers of data tables it materialises (table_code_pointers)."""
    page, found = {}, []
    for pc in range(entry, end, 4):
        w = R.word(pc)
        if (w & 0x9f000000) == 0x90000000:
            imm = R.sext((((w >> 5) & 0x7ffff) << 2) | ((w >> 29) & 3), 21)
            page[w & 31] = (pc, (pc & ~0xfff) + (imm << 12))
        elif (w & 0xffc00000) == 0x91000000:
            hit = page.get((w >> 5) & 31)
            if hit and pc - hit[0] <= 32:
                a = hit[1] + ((w >> 10) & 0xfff)
                found.append(a)
                if not SDK_BASE <= a < SDK_BASE + R.TEXT_SIZE:          # a data address: a table of code pointers?
                    found.extend(table_code_pointers(a))
        elif (w & 0x9f000000) == 0x10000000:
            found.append(pc + R.sext((((w >> 5) & 0x7ffff) << 2) | ((w >> 29) & 3), 21))
        elif (w & 0xffc00000) == 0xf9400000:
            hit = page.get((w >> 5) & 31)
            if hit and pc - hit[0] <= 32:
                t = CODE_SLOTS.get(hit[1] + ((w >> 10) & 0xfff) * 8)
                if t is not None:
                    found.append(t)
    return [t for t in found if t in starts_set and not entry <= t < end]


def lift_function(env, entry):
    end = env.extent(entry)
    fn = R.Fn(entry, end, env)
    lines, bad = [], []
    last_noreturn = False
    for pc in range(entry, end, 4):
        w = R.word(pc)
        env.noreturn_stmt = False
        try:
            stmt = fn.lift(w, pc)
        except R.Unimpl as e:
            stmt = 'guest_unimpl(0x%xULL, 0x%08xU, "%s");' % (pc, w, e)
            bad.append((pc, w, str(e)))
        last_noreturn = env.noreturn_stmt
        lines.append((pc, stmt))
    out = ['static void f_%x(Ctx *c) {' % entry, '  c->fn = 0x%xULL;' % entry]
    if fn.has_br:
        out.append('  uint64_t jt = 0;')
    out += ['L_%x: %s' % (pc, s) for pc, s in lines]
    lastw = R.word(end - 4)
    falls = not lines[-1][1].startswith(TAILS) and not last_noreturn
    if (lastw & 0xfc000000) == 0x14000000 or lastw == 0xd65f03c0 or (lastw & 0xfffffc1f) == 0xd61f0000:
        falls = False
    if falls and end in env.ends:
        env.wanted.setdefault(end, None)
        out.append('  f_%x(c); return;' % end)
    elif falls:
        out.append('  guest_trap("fell off the end of an SDK function", 0x%xULL); return;' % end)
    else:
        out.append('  return;')
    if fn.has_br:
        out.append('L_dispatch: switch (jt) {')
        out += ['  case 0x%xULL: goto L_%x;' % (pc, pc) for pc, _ in lines]
        out.append('  default: call_indirect(c, jt); return; }')
    out.append('}')
    for t in scan_code_refs(entry, end, env.starts_set):
        env.wanted.setdefault(t, None)
    return '\n'.join(out), len(lines), bad, any('guest_svc' in s for _, s in lines)


def data_addresses(start, end):
    """Addresses in the sdk's data segment (.data / .bss) that the code in [start, end) computes with ADRP + ADD."""
    data_start = SDK_BASE + MOD.sec['.data'][0]
    page, out = {}, set()
    for pc in range(start, end, 4):
        w = R.word(pc)
        if (w & 0x9f000000) == 0x90000000:                                  # adrp
            imm = ((w >> 29) & 3) | (((w >> 5) & 0x7ffff) << 2)
            page[w & 31] = (pc & ~0xfff) + (R.sext(imm, 21) << 12)
        elif (w & 0xffc00000) == 0x91000000 and (w >> 5) & 31 in page:      # add xd, xn, #imm12
            a = page[(w >> 5) & 31] + ((w >> 10) & 0xfff)
            if a >= data_start:
                out.add(a)
    return out


def selected_ctors(rules):
    """SDK static constructors (.init_array, in order) that directly call (BL / B, through PLT stubs) a function whose
    name matches a `ctors` rule, or that compute the address of a data object which a function matching a `ctorsdata`
    rule also computes (the constructor sets up a global those functions use): on a console the SDK module runs them
    before the game starts."""
    if not (rules.ctors or rules.ctorsdata) or 25 not in MOD.dt:
        return []
    rel = {off: add for off, info, add in MOD.relocs() if (info & 0xffffffff) == 0x403}
    entries = [rel.get(MOD.dt[25] + 8 * k, 0) for k in range(MOD.dt[27] // 8)]
    env = Env(rules, [])
    env.compute_starts()
    used = set()
    if rules.ctorsdata:
        for s, e in env.ends.items():
            if any(c.search(n) for n in by_addr.get(s, []) for c in rules.ctorsdata):
                used |= data_addresses(s, e)
    out = []
    for k, e in enumerate(entries):
        a = SDK_BASE + e
        if a not in env.ends:
            continue
        hit = bool(used & data_addresses(a, env.ends[a]))
        for pc in range(a, env.ends[a], 4):
            w = R.word(pc)
            if (w & 0x7c000000) == 0x14000000:
                t, imp = env.resolve(pc + R.sext(w & 0x3ffffff, 26) * 4)
                if t is not None and any(c.search(n) for n in by_addr.get(t, []) for c in rules.ctors):
                    hit = True
        if hit:
            out.append((k, a))
    return out


def closure(rules, root_names, limit=10 ** 9, extra_roots=()):
    roots = [SDK_BASE + syms[n][0] for n in root_names] + list(extra_roots)
    env = Env(rules, roots)
    env.compute_starts()
    env.starts_set = set(env.starts)
    for r in roots:
        env.wanted.setdefault(r, None)
    bodies, done, total, allbad, svc = [], [], 0, [], []
    seen = set()
    while True:
        todo = [a for a in env.wanted if a not in seen]
        if not todo or total > limit:
            break
        for a in todo:
            if a not in env.ends:
                seen.add(a)
                allbad.append((a, 0, 'not a listed function start'))
                continue
            body, n, bad, has_svc = lift_function(env, a)
            bodies.append(body)
            done.append(a)
            seen.add(a)
            total += n
            allbad += bad
            if has_svc:
                svc.append(a)
    return env, bodies, done, total, allbad, svc


def label(a):
    return by_addr.get(a, ['sub_%x' % (a - SDK_BASE)])[0]


def write_if_changed(path, text):
    if not os.path.exists(path) or open(path, encoding='utf-8').read() != text:
        with open(path, 'w', encoding='utf-8', newline='\n') as f:
            f.write(text)


def main():
    rules = Rules()
    if sys.argv[1] == 'closure':
        for n in [n for n in syms if sys.argv[2] in n and syms[n][2] == 2][:20]:
            env, bodies, done, total, bad, svc = closure(rules, [n], 200000)
            print('%-90s fns %5d insns %7d bad %d svc %d host %d' % (n[:90], len(done), total, len(bad), len(svc), len(env.host_names)))
        return
    root_names = rules.root_names()
    ctors = selected_ctors(rules)
    droots = rules.data_roots()
    env, bodies, done, total, bad, svc = closure(rules, root_names, extra_roots=[a for _, a in ctors] + droots)
    print('code pointers in matching data objects (datafuncs): %d' % len(droots))
    lines = ['/* generated by tools/lifter/sdklift.py from the dump\'s sdk.elf -- do not edit */',
             '#include "runtime.h"', '#include "sdklift.h"', '',
             'static void sdk_call_indirect(Ctx *c, uint64_t a);', '#define call_indirect sdk_call_indirect']
    lines += ['static void f_%x(Ctx *c);' % a for a in done]
    lines += ['', '\n\n'.join(bodies), '', '#undef call_indirect', 'const char *const sdk_host_names[] = {']
    lines += ['  "%s",' % n for n in env.host_names] + ['  0};', 'const int sdk_host_name_count = %d;' % len(env.host_names),
                                                       'const SdkFn sdk_fns[] = {']
    lines += ['  {0x%xULL, f_%x, "%s"},' % (a, a, label(a)) for a in sorted(done)]
    lines += ['};', 'const int sdk_fn_count = %d;' % len(done),
              '/* SDK static constructors to run before the game starts (`ctors` rules; .init_array order and index) */',
              'const uint64_t sdk_ctors[] = {' + ''.join(' 0x%xULL, /* [%d] */' % (a, k) for k, a in ctors) + ' 0 };',
              'const int sdk_ctor_count = %d;' % len(ctors),
              'static void sdk_call_indirect(Ctx *c, uint64_t a) { sdk_dispatch(c, a); }', '']
    write_if_changed(os.path.join(GEN, 'sdklift_gen.c'), '\n'.join(lines))
    # imports whose handler becomes the lifted SDK code (nnservices.c install_lifted)
    install = [n for n in root_names if n in GAME_IMPORTS]
    write_if_changed(os.path.join(GEN, 'sdklift_install.h'),
                     '/* generated by tools/lifter/sdklift.py: imports answered by the lifted SDK code */\n'
                     'static const char *const sdk_install_names[] = {\n' + ''.join('  "%s",\n' % n for n in install) + '  0 };\n')
    with open(os.path.join(GEN, 'sdklift_report.txt'), 'w', encoding='utf-8') as f:
        f.write('roots %d, lifted %d functions, %d instructions, untranslated %d, functions with svc %d\nhost calls:\n'
                % (len(root_names), len(done), total, len(bad), len(svc)))
        f.writelines('  %s%s\n' % (n, '' if n in GAME_IMPORTS else '   (NOT a game import: no host handler by default)') for n in env.host_names)
        f.write('functions with svc:\n')
        f.writelines('  %x %s\n' % (a - SDK_BASE, label(a)) for a in svc)
        f.write('untranslated:\n')
        f.writelines('  %x %08x %s\n' % (pc - SDK_BASE if pc else 0, w, why) for pc, w, why in bad)
    print('roots %d   functions lifted %d   instructions %d   untranslated %d   with svc %d   host names %d (not game imports: %d)   static constructors to run: %d %s'
          % (len(root_names), len(done), total, len(bad), len(svc), len(env.host_names),
             sum(1 for n in env.host_names if n not in GAME_IMPORTS), len(ctors), [k for k, _ in ctors]))
    print('report: %s' % os.path.join(GEN, 'sdklift_report.txt'))


if __name__ == '__main__':
    main()
