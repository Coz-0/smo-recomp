"""Generate the per-build tables the host runtime needs, from the user's own converted dump, into build/gen/.

    python tools/lifter/gen_tables.py

Writes (everything derived from the dump; build/ is ignored by git):
  image.h            image layout of main and sdk (segment ends, relocation tables, .dynsym, .init_array, entry
                     points), and GAME_SYM_<name> addresses of the game functions the runtime calls by name (in an
                     update's main without symbols: matched with the base executable's, see find_sampler_sort),
                     GAME_VSYM_<name> of the virtual functions it watches for (from RTTI vtables, see find_vsym), and
                     GAME_FN_<name> of the game functions its test aids call (matched likewise, see find_func).
  imports_all.txt    names main imports (undefined dynamic symbols), symbol-table order
  sdkdata.c          values of the data symbols main imports from sdk (nn::util tables, nn::hid NpadId constants),
                     copied out of the real sdk module
  sdklift_install.h  imports replaced by lifted SDK code, and
  sdklift_gen.c      the lifted SDK functions: empty stand-ins only until tools/lifter/sdklift.py has written them
"""
import bisect
import csv
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, 'tools', 'extract'))
sys.path.insert(0, os.path.join(REPO, 'tools'))
import functions as F                                     # noqa: E402  (the ELF reader of tools/extract)
import target as T                                        # noqa: E402

DUMP = os.path.join(T.DUMP, 'elf')
BASE_MAIN = T.base_elf('main')
OUT = T.build('gen')
GUEST_BASE = 0x7100000000
SDK_BASE = 0x7500000000
# game functions the runtime calls by name (the console's own allocator lives in main for this game)
GAME_SYMS = ['malloc', 'free', 'calloc', 'realloc', 'aligned_alloc', 'nninitStartup', 'nnMain',
             '_ZdlPv']               # operator delete(void *): frees what the game's operator new (sead heaps) allocated
# Optional: runtime/game/smo_fast.c's sampler list sort of agl::driver::NVNMgr in host code. A main without these symbols
# (1.4.1's is stripped) gets them from the base executable's by matching (find_sampler_sort), or 0 when that fails, and
# smo_fast.c then leaves the game's own code alone. Order: the sort, registerSampler_, the key comparison.
GAME_SYMS_OPTIONAL = ['_ZN4sead12PtrArrayImpl4sortEPFiPKvS2_E', '_ZN3agl6driver6NVNMgr16registerSampler_EiPK10NVNsamplerj',
                      '_ZN3agl6driver6NVNMgr18compareSamplerKey_EPKNS1_10SamplerKeyES4_']
# Virtual functions the runtime watches for (GAME_VSYM_<name>): (class as RTTI names it, mangled function, name). In a
# main without symbols they come from the class's vtable, in the slot that holds them in the base executable's
# (find_vsym), or 0. StageScene::init: a stage scene starts (test aid SMO_START_STAGE, runtime/game/smo_nerve.c).
GAME_VSYMS = [('10StageScene', '_ZN10StageScene4initERKN2al13SceneInitInfoE', 'StageScene_init')]
# Game functions the runtime calls (GAME_FN_<name>): (mangled function, name). A main without symbols gets each from the
# base executable's (find_func), or 0. ChangeStageInfo's constructor and GameDataFunction::tryChangeNextStage: a stage
# change of the game's own, as a door makes it (test aid SMO_STAGE_LIST, runtime/game/smo_nerve.c).
GAME_FUNCS = [('_ZN15ChangeStageInfoC1EPK14GameDataHolderPKcS4_biNS_15SubScenarioTypeE', 'ChangeStageInfo_ctor'),
              ('_ZN16GameDataFunction18tryChangeNextStageE20GameDataHolderWriterPK15ChangeStageInfo', 'tryChangeNextStage')]


def code_words(m, start, size):
    return list(struct.unpack_from('<%dI' % (size // 4), m.d, F.OFF + start))


def adrp_page(a, w):
    return (a & ~0xfff) + (F.sext(((w >> 29) & 3) | (((w >> 5) & 0x7ffff) << 2), 21) << 12)


def masked(words):
    """AArch64 instruction words with the fields that depend on where code and data lie cleared: branch and literal
    offsets, ADR / ADRP immediates, and the low 12 bits an ADD or a load / store adds to a register an ADRP set."""
    out, pages = [], set()
    for w in words:
        if w & 0x7c000000 == 0x14000000:                 # B, BL
            w &= 0xfc000000
        elif w & 0xff000010 == 0x54000000 or w & 0x7e000000 == 0x34000000 or w & 0x3b000000 == 0x18000000:
            w &= 0xff00001f                              # B.cond, CBZ / CBNZ, LDR (literal)
        elif w & 0x7e000000 == 0x36000000:               # TBZ / TBNZ
            w &= 0xfff8001f
        elif w & 0x1f000000 == 0x10000000:               # ADR, ADRP
            if w & 0x80000000:
                pages.add(w & 31)
            w &= 0x9f00001f
        elif (w & 0x7f800000 == 0x11000000 or w & 0x3b000000 == 0x39000000) and (w >> 5) & 31 in pages:
            w &= ~(0xfff << 10) & 0xffffffff
        out.append(w)
    return out


def pointer_loads(m, target):
    """Addresses of the 64-bit loads (LDR after an ADRP of the same register, within 16 instructions) of a pointer slot
    that holds `target` (a RELATIVE relocation with that addend)."""
    slots = {off for off, info, add in m.relocs() if info & 0xffffffff == 1027 and add == target}
    ws, out = m.words(), []
    for i, w in enumerate(ws):
        if w & 0x9f000000 != 0x90000000:
            continue
        page, rd = adrp_page(i * 4, w), w & 31
        for k in range(i + 1, min(i + 17, len(ws))):
            v = ws[k]
            if v & 0xffc00000 == 0xf9400000 and (v >> 5) & 31 == rd:
                if page + ((v >> 10) & 0xfff) * 8 in slots:
                    out.append(k * 4)
                break
    return out


def caller_and_callee(m, funcs, keycmp):
    """registerSampler_ = the one function that loads a pointer to the key comparison; the sort = the first call after
    that load. (start, size of the caller, callee) or None."""
    starts = [s for s, _ in funcs]
    found = set()
    loads = pointer_loads(m, keycmp)
    for a in loads:
        i = bisect.bisect_right(starts, a) - 1
        found.add(funcs[i] if i >= 0 and a < funcs[i][0] + funcs[i][1] else None)
    if len(found) != 1 or None in found:
        return None
    (start, size), = found
    for q in range(loads[0], start + size, 4):
        w = m.u32(q)
        if w & 0xfc000000 == 0x94000000:                 # BL
            return start, size, q + F.sext(w & 0x3ffffff, 26) * 4
    return None


def indirect_calls(m, start, size):
    """(BL count, BLR count) of a function."""
    ws = code_words(m, start, size)
    return sum(1 for w in ws if w & 0xfc000000 == 0x94000000), sum(1 for w in ws if w & 0xfffffc1f == 0xd63f0000)


def find_sampler_sort(main_m):
    """GAME_SYMS_OPTIONAL in a main without symbols (1.4.1), from the base executable's (build/dump/base): the key
    comparison is the only function of its size whose masked instructions equal the base's (which no other base function
    has); registerSampler_ is the only function that loads a pointer to it, and the sort is the first call after that
    load. The same rule run on the base must give the base's own symbols, and the sort found must, like the base's,
    call only through pointers (the comparator). The sort's algorithm itself is checked at run time with
    SMO_FAST_CHECK=1 (runtime/game/smo_fast.c). Returns ({name: address}, None) or ({}, why not)."""
    if not os.path.exists(BASE_MAIN):
        return {}, 'the base executable is not in build/dump/base'
    base = F.Module(BASE_MAIN)
    syms = {n: (v, s) for n, t, b, sh, v, s in base.symbols() if sh and n in GAME_SYMS_OPTIONAL}
    if len(syms) != len(GAME_SYMS_OPTIONAL):
        return {}, 'the base executable does not name them'
    sort_n, reg_n, cmp_n = GAME_SYMS_OPTIONAL
    base_funcs = sorted({(v, s) for n, t, b, sh, v, s in base.symbols() if sh and t == 2 and s})
    rows = csv.DictReader(open(T.build('functions', 'main_functions.tsv'), encoding='utf-8'), delimiter='\t')
    funcs = sorted((int(r['start'], 16), int(r['size'])) for r in rows)
    cv, cs = syms[cmp_n]
    pattern = masked(code_words(base, cv, cs))
    if [v for v, s in base_funcs if s == cs and masked(code_words(base, v, s)) == pattern] != [cv]:
        return {}, 'the key comparison\'s instructions are not unique in the base executable'
    same = [st for st, sz in funcs if sz == cs and masked(code_words(main_m, st, sz)) == pattern]
    if len(same) != 1:
        return {}, '%d functions have the key comparison\'s instructions' % len(same)
    on_base = caller_and_callee(base, base_funcs, cv)
    if not on_base or on_base[0] != syms[reg_n][0] or on_base[2] != syms[sort_n][0]:
        return {}, 'the rule does not give the base executable\'s own registerSampler_ and sort'
    found = caller_and_callee(main_m, funcs, same[0])
    if not found:
        return {}, 'no single function loads a pointer to the key comparison'
    sort_size = next((sz for st, sz in funcs if st == found[2]), 0)
    if not sort_size or indirect_calls(main_m, found[2], sort_size) != indirect_calls(base, *syms[sort_n]):
        return {}, 'the function called after the load does not call like the base\'s sort'
    return {sort_n: found[2], reg_n: found[0], cmp_n: same[0]}, None


def vtable_slots(m, cls):
    """The virtual functions of the primary vtable of the class whose RTTI name is cls (e.g. 10StageScene), in slot
    order (None for a slot that is not a function of the module), or None when there is not exactly one. A typeinfo is a
    pointer to a __cxxabiv1 class_type_info vtable (a symbol relocation) followed by its name (a RELATIVE relocation); a
    vtable is offset-to-top (not relocated; 0 for the primary one), the typeinfo, then the functions."""
    syms = m.symbols()
    rel, symrel = {}, {}
    for off, info, add in m.relocs():
        kind = info & 0xffffffff
        if kind == 0x403:                                 # R_AARCH64_RELATIVE
            rel[off] = add
        elif kind in (0x101, 0x401):                      # R_AARCH64_ABS64, GLOB_DAT
            symrel[off] = syms[info >> 32][0]
    tinfo = [off for off, n in symrel.items() if n.startswith('_ZTVN10__cxxabiv1') and n.endswith('class_type_infoE')
             and off + 8 in rel and m.cstr(rel[off + 8]) == cls]
    if len(tinfo) != 1:
        return None
    vts = [o - 8 for o, p in rel.items() if p == tinfo[0] and o - 8 not in rel and o - 8 not in symrel
           and struct.unpack_from('<q', m.d, F.OFF + o - 8)[0] == 0]
    if len(vts) != 1:
        return None
    out, a = [], vts[0] + 16
    while a in rel or a in symrel:
        out.append(rel.get(a))
        a += 8
    return out


def find_vsym(main_m, defined, cls, name):
    """(address, how) of virtual function `name` of class cls: main's own symbol (checked against its vtable), else the
    function in the slot of main's vtable for the class that holds it in the base executable's; (0, why not)."""
    base = F.Module(BASE_MAIN) if os.path.exists(BASE_MAIN) else None
    ref = (base, {n: v for n, t, b, sh, v, s in base.symbols() if sh}) if base else (main_m, defined)
    slots_ref = vtable_slots(ref[0], cls)
    if name not in ref[1] or not slots_ref or slots_ref.count(ref[1][name]) != 1:
        return 0, 'the base executable\'s vtable of %s does not hold it once' % cls
    slots = vtable_slots(main_m, cls)
    k = slots_ref.index(ref[1][name])
    if not slots or k >= len(slots) or not slots[k]:
        return 0, 'no vtable of %s in this main.elf' % cls
    if name in defined and defined[name] != slots[k]:
        return 0, 'the vtable slot does not hold the symbol'
    return slots[k], 'own symbol' if name in defined else 'vtable slot %d, as in the base executable' % k


def bl_targets(m, start, size):
    """The targets of a function's BL instructions, in order."""
    return [start + 4 * i + F.sext(w & 0x3ffffff, 26) * 4 for i, w in enumerate(code_words(m, start, size))
            if w & 0xfc000000 == 0x94000000]


class FuncMatcher:
    """Functions of a main without symbols found from the base executable's (build/dump/base), which names them."""

    def __init__(self, main_m):
        self.main, self.base = main_m, F.Module(BASE_MAIN)
        self.base_sym = {n: (v, s) for n, t, b, sh, v, s in self.base.symbols() if sh and t == 2 and s}
        base_funcs = sorted({vs for vs in self.base_sym.values()})
        rows = csv.DictReader(open(T.build('functions', 'main_functions.tsv'), encoding='utf-8'), delimiter='\t')
        funcs = sorted((int(r['start'], 16), int(r['size'])) for r in rows)
        self.base_size, self.size = dict(base_funcs), dict(funcs)
        self.base_by_size, self.by_size = {}, {}
        for v, s in base_funcs:
            self.base_by_size.setdefault(s, []).append(v)
        for v, s in funcs:
            self.by_size.setdefault(s, []).append(v)
        self.calls, self.base_calls, self.equal_of = None, None, {}

    def equal(self, v):
        """The one function of this main whose masked instructions equal those of base function v, which no other base
        function has; else None."""
        if v not in self.equal_of:
            s, r = self.base_size.get(v), None
            if s:
                pat = masked(code_words(self.base, v, s))
                if [o for o in self.base_by_size[s] if masked(code_words(self.base, o, s)) == pat] == [v]:
                    same = [o for o in self.by_size.get(s, []) if masked(code_words(self.main, o, s)) == pat]
                    r = same[0] if len(same) == 1 else None
            self.equal_of[v] = r
        return self.equal_of[v]

    def by_calls(self, v, s):
        """The one function of this main of about the size of base function v (within a quarter) whose calls (BL)
        go to the same functions as v's wherever the callee is matched by equal(), at least two of them and at least
        half; the same rule over the base executable's functions must give v alone. (address, matched calls) or None."""
        want = bl_targets(self.base, v, s)
        mapped = [self.equal(t) for t in want]
        known = [i for i, x in enumerate(mapped) if x is not None]
        if len(known) < 2 or 2 * len(known) < len(want):
            return None
        if self.calls is None:
            self.calls = {o: bl_targets(self.main, o, sz) for o, sz in self.size.items()}
            self.base_calls = {o: bl_targets(self.base, o, sz) for o, sz in self.base_size.items()}

        def fits(calls, targets):
            return len(calls) == len(want) and all(calls[i] == targets[i] for i in known)
        if [o for o, c in self.base_calls.items() if fits(c, want)] != [v]:
            return None
        hits = [o for o, c in self.calls.items() if fits(c, mapped) and abs(self.size[o] - s) <= s // 4]
        return (hits[0], len(known)) if len(hits) == 1 else None


def find_func(main_m, defined, name, matcher):
    """(address, how) of game function `name`: main's own symbol, else (a main without symbols) the one function whose
    masked instructions equal the base executable's function of that name, else the one whose calls match its calls
    (FuncMatcher.by_calls); (0, why not)."""
    if name in defined:
        return defined[name], 'own symbol'
    if not matcher:
        return 0, 'the base executable is not in build/dump/base'
    if name not in matcher.base_sym:
        return 0, 'the base executable does not name it'
    v, s = matcher.base_sym[name]
    a = matcher.equal(v)
    if a is not None:
        return a, 'matched with the base executable\'s instructions'
    hit = matcher.by_calls(v, s)
    if hit:
        return hit[0], 'matched with the base executable\'s by %d of the %d functions it calls' % (hit[1], len(bl_targets(matcher.base, v, s)))
    return 0, 'no single function of this main.elf matches the base executable\'s'


def layout(m):
    dv, dsz = m.sec['.data'][0], m.sec['.data'][1] + m.sec['.dynamic'][1] + m.sec['.data.1'][1]
    file_image = m.sec['.data.1'][0] + m.sec['.data.1'][1]
    bss_end = m.sec['.bss'][0] + m.sec['.bss'][1]
    image_size = (max(file_image, bss_end) + 0xfff) & ~0xfff
    return dv, file_image, image_size


def write(name, text):
    p = os.path.join(OUT, name)
    old = open(p, encoding='utf-8').read() if os.path.exists(p) else None
    if old != text:                                       # unchanged files keep their time stamp (incremental builds)
        with open(p, 'w', encoding='utf-8', newline='\n') as f:
            f.write(text)
    return p


def main():
    os.makedirs(OUT, exist_ok=True)
    main_m = F.Module(os.path.join(DUMP, 'main.elf'))
    sdk_m = F.Module(os.path.join(DUMP, 'sdk.elf'))
    syms = main_m.symbols()
    defined = {}
    for n, t, b, sh, v, s in syms:
        if sh and n:
            defined.setdefault(n, v)
    ro_end, file_image, image_size = layout(main_m)
    _, _, sdk_image_size = layout(sdk_m)
    dt = main_m.dt
    package = json.load(open(os.path.join(REPO, 'build', 'dump', 'version.json')))['update']
    build_id = package['modules']['main']['build_id']
    sdk = package['program_sdk_version']                                               # "7.3.2.0"
    sdk_version = sum(int(x) << (24 - 8 * i) for i, x in enumerate(sdk.split('.')[:4]))
    elf_from_bin = os.path.relpath(os.path.join(DUMP, 'main.elf'), T.build('bin')).replace('/', '\\')
    h = ['/* generated by tools/lifter/gen_tables.py from main.elf / sdk.elf of the dump -- do not edit */',
         '#ifndef SMO_IMAGE_H', '#define SMO_IMAGE_H', '',
         '#define GAME_VERSION "%s"' % T.VERSION,
         '#define SDK_VERSION 0x%08xu      /* the NintendoSDK of the game\'s program NCA (%s): layouts that differ by SDK */'
         % (sdk_version, sdk),
         '#define GAME_ELF_DEFAULT "%s"      /* main.elf from the exe\'s folder (sdk.elf beside it) */'
         % elf_from_bin.replace('\\', '\\\\'),
         '#define GAME_BUILD_ID "%s"' % build_id,
         '#define GUEST_FILE_IMAGE 0x%xULL      /* bytes of the image stored in main.elf (text .. end of data) */' % file_image,
         '#define GUEST_IMAGE_SIZE 0x%xULL      /* mapped size: up to the end of .bss, page aligned */' % image_size,
         '#define GUEST_RO_END 0x%xULL          /* start of the data segment: text + read-only data end here */' % ro_end,
         '#define GUEST_RELA 0x%xULL' % dt[7], '#define GUEST_RELASZ 0x%xULL' % dt[8],
         '#define GUEST_JMPREL 0x%xULL' % dt.get(23, 0), '#define GUEST_PLTRELSZ 0x%xULL' % dt.get(2, 0),
         '#define GUEST_DYNSYM 0x%xULL' % dt[6], '#define GUEST_DYNSTR 0x%xULL' % dt[5],
         '#define GUEST_INIT_ARRAY (GUEST_BASE + 0x%xULL)' % dt[25], '#define GUEST_INIT_ARRAY_SIZE 0x%xULL' % dt[27],
         '#define GUEST_TEXT_SIZE 0x%xULL' % main_m.text_end, '']
    for n in GAME_SYMS:
        if n not in defined:
            raise SystemExit('main.elf does not define %s' % n)
        h.append('#define GAME_SYM_%s (GUEST_BASE + 0x%xULL)' % (n, defined[n]))
    missing = [n for n in GAME_SYMS_OPTIONAL if n not in defined]
    matched, why = find_sampler_sort(main_m) if missing else ({}, None)
    for n in GAME_SYMS_OPTIONAL:
        if n in defined:
            h.append('#define GAME_SYM_%s (GUEST_BASE + 0x%xULL)' % (n, defined[n]))
        elif n in matched:
            h.append('#define GAME_SYM_%s (GUEST_BASE + 0x%xULL)      /* matched with the base executable\'s */'
                     % (n, matched[n]))
        else:
            h.append('#define GAME_SYM_%s 0ULL      /* not in this main.elf */' % n)
    vsyms = []
    for cls, n, short in GAME_VSYMS:
        a, how = find_vsym(main_m, defined, cls, n)
        vsyms.append('%s %s' % (short, '0x%x (%s)' % (a, how) if a else 'not found: ' + how))
        h.append('#define GAME_VSYM_%s %s      /* %s */' % (short, '(GUEST_BASE + 0x%xULL)' % a if a else '0ULL', how))
    fns = []
    matcher = FuncMatcher(main_m) if os.path.exists(BASE_MAIN) and any(n not in defined for n, _ in GAME_FUNCS) else None
    for n, short in GAME_FUNCS:
        a, how = find_func(main_m, defined, n, matcher)
        fns.append('%s %s' % (short, '0x%x (%s)' % (a, how) if a else 'not found: ' + how))
        h.append('#define GAME_FN_%s %s      /* %s */' % (short, '(GUEST_BASE + 0x%xULL)' % a if a else '0ULL', how))
    h += ['', '#define SDK_IMAGE_SIZE_GEN 0x%xULL' % sdk_image_size,
          '#define SDK_TEXT_SIZE_GEN 0x%xULL' % sdk_m.text_end, '', '#endif', '']
    write('image.h', '\n'.join(h))

    imports = [n for n, t, b, sh, v, s in syms[1:] if sh == 0 and n]
    write('imports_all.txt', ''.join(n + '\n' for n in imports))

    sdk_objs = {}
    for n, t, b, sh, v, s in sdk_m.symbols():
        if sh and t == 1:
            sdk_objs[n] = (v, s)
    found = []
    for n in imports:
        if n.startswith(('_ZN2nn4util', '_ZN2nn3hid6NpadId', '_ZNK2nn4util')) and n in sdk_objs and 0 < sdk_objs[n][1] <= 1 << 20:
            v, s = sdk_objs[n]
            found.append((n, sdk_m.d[0x1000 + v:0x1000 + v + s]))
    c = ['/* generated by tools/lifter/gen_tables.py from the real sdk module -- do not edit */', '#include "hostrt.h"', '']
    for k, (n, data) in enumerate(found):
        c.append('static const unsigned char d%d[] = {%s};' % (k, ','.join(str(x) for x in data)))
    c += ['', 'const SdkData sdk_data[] = {'] + ['  { "%s", d%d, %d },' % (n, k, len(data)) for k, (n, data) in enumerate(found)]
    if not found:
        c.append('  { 0, 0, 0 },')
    c += ['};', 'const int sdk_data_count = %d;' % len(found), '']
    write('sdkdata.c', '\n'.join(c))

    # empty stand-ins, only where tools/lifter/sdklift.py has not written the real files (never overwrite them)
    if not os.path.exists(os.path.join(OUT, 'sdklift_install.h')):
        write('sdklift_install.h', '/* generated: EMPTY (run tools/lifter/sdklift.py gen) */\n'
                                   'static const char *const sdk_install_names[] = { 0 };\n')
    if not os.path.exists(os.path.join(OUT, 'sdklift_gen.c')):
        write('sdklift_gen.c', '/* generated by gen_tables.py: EMPTY set of lifted SDK functions (run tools/lifter/sdklift.py gen) */\n'
                               '#include "runtime.h"\n#include "sdklift.h"\n\n'
                               'const char *const sdk_host_names[] = { 0 };\nconst int sdk_host_name_count = 0;\n'
                               'const SdkFn sdk_fns[] = { {0, 0, 0} };\nconst int sdk_fn_count = 0;\n'
                               'const uint64_t sdk_ctors[] = { 0 };\nconst int sdk_ctor_count = 0;\n')
    print('image: file 0x%x, mapped 0x%x, ro end 0x%x, init_array %d entries; sdk mapped 0x%x'
          % (file_image, image_size, ro_end, dt[27] // 8, sdk_image_size))
    print('imports: %d   sdk data symbols: %d   game symbols: %s' % (len(imports), len(found), ', '.join(GAME_SYMS)))
    if matched:
        print('not in this main.elf, matched with the base executable\'s: %s' % ', '.join(
            '%s 0x%x' % (n, matched[n]) for n in GAME_SYMS_OPTIONAL if n in matched))
    elif missing:
        print('not in this main.elf (their fast path stays off: %s): %s' % (why, ', '.join(missing)))
    print('virtual functions: %s' % '; '.join(vsyms))
    print('game functions: %s' % '; '.join(fns))
    print('wrote', OUT)


if __name__ == '__main__':
    main()
