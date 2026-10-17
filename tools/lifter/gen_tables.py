"""Generate the per-build tables the host runtime needs, from the user's own converted dump, into build/gen/.

    python tools/lifter/gen_tables.py

Writes (everything derived from the dump; build/ is ignored by git):
  image.h            image layout of main and sdk (segment ends, relocation tables, .dynsym, .init_array, entry
                     points), and GAME_SYM_<name> addresses of the game functions the runtime calls by name (in an
                     update's main without symbols: matched with the base executable's, see find_sampler_sort).
                     Replaces the 13.0.4 constants ssbu-recomp hard-coded in hostrt.c / boot.c / sdklift.h / libcxx.c.
  imports_all.txt    names main imports (undefined dynamic symbols), symbol-table order (as ssbu's gen_imports.py)
  sdkdata.c          values of the data symbols main imports from sdk (nn::util tables, nn::hid NpadId constants),
                     copied out of the real sdk module (as ssbu's gen_sdkdata.py)
  fake_whitelist.h   strict-audit white list: EMPTY at first (nothing is proven correct-by-semantics for SMO yet)
  fake_openlist.h    strict-audit open-row list: EMPTY
  sdklift_install.h  imports replaced by lifted SDK code: EMPTY until sdklift is ported to SMO's sdk
  sdklift_gen.c      lifted SDK functions: an EMPTY set (the image is still mapped by sdklift_rt.c)
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
import target as T                                        # noqa: E402  (SMO_TARGET: which package, which build/)

DUMP = os.path.join(T.DUMP, 'elf')
BASE_MAIN = os.path.join(REPO, 'build', 'dump', 'base', 'elf', 'main.elf')
OUT = T.build('gen')
GUEST_BASE = 0x7100000000
SDK_BASE = 0x7500000000
# game functions the runtime calls by name (the console's own allocator lives in main for this game)
GAME_SYMS = ['malloc', 'free', 'calloc', 'realloc', 'aligned_alloc', 'nninitStartup', 'nnMain',
             '_ZdlPv']               # operator delete(void *): frees what the game's operator new (sead heaps) allocated
# Optional: runtime/smo_fast.c's sampler list sort of agl::driver::NVNMgr in host code. A main without these symbols
# (1.4.1's is stripped) gets them from the base executable's by matching (find_sampler_sort), or 0 when that fails, and
# smo_fast.c then leaves the game's own code alone. Order: the sort, registerSampler_, the key comparison.
GAME_SYMS_OPTIONAL = ['_ZN4sead12PtrArrayImpl4sortEPFiPKvS2_E', '_ZN3agl6driver6NVNMgr16registerSampler_EiPK10NVNsamplerj',
                      '_ZN3agl6driver6NVNMgr18compareSamplerKey_EPKNS1_10SamplerKeyES4_']


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
    SMO_FAST_CHECK=1 (runtime/smo_fast.c). Returns ({name: address}, None) or ({}, why not)."""
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
    package = json.load(open(os.path.join(REPO, 'build', 'dump', 'version.json')))[T.DUMP_KIND]
    build_id = package['modules']['main']['build_id']
    sdk = next(n['sdk_version'] for n in package['ncas'] if n['type'] == 'Program')     # "7.3.2.0"
    sdk_version = sum(int(x) << (24 - 8 * i) for i, x in enumerate(sdk.split('.')[:4]))
    elf_from_bin = os.path.relpath(os.path.join(DUMP, 'main.elf'), T.build('bin')).replace('/', '\\')
    h = ['/* generated by tools/lifter/gen_tables.py from main.elf / sdk.elf of the dump -- do not edit */',
         '#ifndef SMO_IMAGE_H', '#define SMO_IMAGE_H', '',
         '#define GAME_VERSION "%s"' % T.TARGET,
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

    for name, var in (('fake_whitelist.h', 'audit_whitelist'), ('fake_openlist.h', 'audit_openlist')):
        write(name, '/* generated: EMPTY (nothing proven for SMO yet) */\nstatic const char *const %s[] = { "" };\n' % var)
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
    print('wrote', OUT)


if __name__ == '__main__':
    main()
