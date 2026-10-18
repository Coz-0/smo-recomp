"""The list of every function of a module, with start/end addresses, the evidence for each boundary, and names
(the module's own dynamic symbols, and for main's nerve states the name RTTI gives).

    python tools/extract/functions.py [--dump build/dump/update] [--out build/functions]

Boundary evidence, per function (column `src`):
    E  .eh_frame FDE (exact start and length, written by the compiler)
    S  dynamic symbol of type FUNC with a size (exact start and length, written by the linker)
    B  target of a BL somewhere in the module
    R  code address stored in data (R_AARCH64_RELATIVE addend into .text)
    T  target of an unconditional B that leaves the function it is in (tail call)
The list is the union of E and S, plus B/R/T targets that no E/S function starts at. The end of a function is the
E or S end when there is one (they must agree, checked), else the next start. Addresses are module-relative
(main is loaded at 0x7100000000 on the console; add that for absolute addresses).

Names of main's nerve states from RTTI (column `rtti_name`; the game's main has almost no symbols): every class whose
typeinfo derives (through single-inheritance bases) from al::Nerve has a vtable whose first function is its
execute(al::NerveKeeper *) const; that function gets the mangled name the class implies. In the base game's main,
which has symbols, these names equal the symbols wherever both exist.

Writes <out>/<module>_functions.tsv (start, end, size, src, end_from, symbol, rtti_name) and <out>/summary.json. Output
is derived from the dump: it stays under build/ (ignored by git).
"""
import argparse
import bisect
import collections
import json
import os
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, 'tools'))
import target as T               # noqa: E402
OFF = 0x1000                     # image offset inside our ELF files (nso.nso_to_elf)


def sext(v, n):
    return v - (1 << n) if v & (1 << (n - 1)) else v


class Module:
    def __init__(self, path):
        self.d = open(path, 'rb').read()
        d = self.d
        shoff, = struct.unpack_from('<Q', d, 0x28)
        shnum, shstrndx = struct.unpack_from('<HH', d, 0x3c)
        secs = [struct.unpack_from('<IIQQQQIIQQ', d, shoff + i * 64) for i in range(shnum)]
        st = secs[shstrndx]
        names = d[st[4]:st[4] + st[5]]
        self.sec = {}
        for s in secs:
            n = names[s[0]:names.index(b'\0', s[0])].decode()
            if n:
                self.sec[n] = (s[3], s[5])           # addr, size
        self.text_end = self.sec['.text'][1]
        # dynamic tags
        dyn, dsz = self.sec['.dynamic']
        self.dt = {}
        for i in range(dsz // 16):
            t, v = struct.unpack_from('<qQ', d, OFF + dyn + i * 16)
            if t == 0:
                break
            self.dt.setdefault(t, v)

    def u32(self, a):
        return struct.unpack_from('<I', self.d, OFF + a)[0]

    def words(self):
        return struct.unpack_from('<%dI' % (self.text_end // 4), self.d, OFF)

    def cstr(self, a):
        e = self.d.index(b'\0', OFF + a)
        return self.d[OFF + a:e].decode('latin-1')

    def symbols(self):
        symtab, strtab = self.dt[6], self.dt[5]
        n = self.sec['.dynsym'][1] // 24
        out = []
        for i in range(n):
            name_off, info, other, shndx, value, size = struct.unpack_from('<IBBHQQ', self.d, OFF + symtab + i * 24)
            out.append((self.cstr(strtab + name_off), info & 0xf, info >> 4, shndx, value, size))
        return out

    def relocs(self):
        if 7 not in self.dt:
            return []
        rela, sz = self.dt[7], self.dt[8]
        return [struct.unpack_from('<QQq', self.d, OFF + rela + i * 24) for i in range(sz // 24)]

    # ---- .eh_frame through .eh_frame_hdr (found from MOD0)
    def fdes(self):
        mod = self.u32(4)
        eh_hdr = mod + struct.unpack_from('<i', self.d, OFF + mod + 0x10)[0]
        ver, ptr_enc, cnt_enc, tbl_enc = self.d[OFF + eh_hdr:OFF + eh_hdr + 4]
        if ver != 1 or cnt_enc != 0x03 or tbl_enc != 0x3b:
            raise ValueError('unexpected .eh_frame_hdr encodings %x %x %x %x' % (ver, ptr_enc, cnt_enc, tbl_enc))
        count = self.u32(eh_hdr + 8)
        cie_cache = {}
        out = []
        for i in range(count):
            loc, fde_rel = struct.unpack_from('<ii', self.d, OFF + eh_hdr + 12 + i * 8)
            fde = eh_hdr + fde_rel
            length = self.u32(fde)
            assert length not in (0, 0xffffffff)
            cie = fde + 4 - self.u32(fde + 4)
            enc = cie_cache.get(cie)
            if enc is None:
                enc = cie_cache[cie] = self._cie_fde_encoding(cie)
            p = fde + 8
            begin, p = self._read_enc(p, enc)
            rng, p = self._read_enc(p, enc & 0x0f)
            assert begin == eh_hdr + loc, ('eh_frame_hdr / FDE disagree', hex(begin), hex(eh_hdr + loc))
            out.append((begin, begin + rng))
        return out

    def _uleb(self, a):
        r = s = 0
        while True:
            b = self.d[OFF + a]
            a += 1
            r |= (b & 0x7f) << s
            s += 7
            if not b & 0x80:
                return r, a

    def _cie_fde_encoding(self, cie):
        p = cie + 8                                   # length, CIE id
        version = self.d[OFF + p]
        p += 1
        e = self.d.index(b'\0', OFF + p)
        aug = self.d[OFF + p:e].decode()
        p = e - OFF + 1
        _, p = self._uleb(p)                          # code alignment
        _, p = self._uleb(p)                          # data alignment (sleb; skipping works the same)
        if version == 1:
            p += 1
        else:
            _, p = self._uleb(p)
        if not aug.startswith('z'):
            return 0
        _, p = self._uleb(p)
        for ch in aug[1:]:
            if ch == 'R':
                return self.d[OFF + p]
            if ch == 'P':
                penc = self.d[OFF + p]
                p += 1
                _, p = self._read_enc(p, penc)
            elif ch == 'L':
                p += 1
        return 0

    def _read_enc(self, p, enc):
        f = enc & 0x0f
        if f == 0x0b:
            v, n = struct.unpack_from('<i', self.d, OFF + p)[0], 4
        elif f == 0x03:
            v, n = self.u32(p), 4
        elif f in (0x00, 0x04, 0x0c):
            v, n = struct.unpack_from('<q', self.d, OFF + p)[0], 8
        else:
            raise ValueError('pointer encoding %x' % enc)
        if (enc & 0x70) == 0x10:
            v += p
        return v, p + n


def analyse(mod):
    words = mod.words()
    text_end = mod.text_end
    exact = {}                                        # start -> {'E': end, 'S': end}
    fdes = mod.fdes()
    for b, e in fdes:
        exact.setdefault(b, {})['E'] = e
    syms = mod.symbols()
    names = collections.defaultdict(list)
    for name, typ, bind, shndx, value, size in syms:
        if shndx and value < text_end:
            if typ == 2 and size:
                exact.setdefault(value, {})['S'] = value + size
            if name:
                names[value].append(name)
    bl, rel, tail = set(), set(), set()
    for i, w in enumerate(words):
        if (w & 0xfc000000) == 0x94000000:
            t = i * 4 + sext(w & 0x3ffffff, 26) * 4
            if 0 <= t < text_end:
                bl.add(t)
    for off, info, addend in mod.relocs():
        if (info & 0xffffffff) == 0x403 and 0 <= addend < text_end and addend % 4 == 0:
            rel.add(addend)

    # exact functions first; disagreements between E and S are reported, E wins (it is what the unwinder uses)
    starts = sorted(exact)
    disagree = [(s, v['E'], v['S']) for s, v in exact.items() if 'E' in v and 'S' in v and v['E'] != v['S']]
    ends = {s: v.get('E', v.get('S')) for s, v in exact.items()}

    def containing(a):
        i = bisect.bisect_right(starts, a) - 1
        if i >= 0 and a < ends[starts[i]]:
            return starts[i]
        return None
    # tail calls: unconditional B whose target is outside the exact function holding the branch
    for i, w in enumerate(words):
        if (w & 0xfc000000) == 0x14000000:
            pc = i * 4
            t = pc + sext(w & 0x3ffffff, 26) * 4
            f = containing(pc)
            if f is not None and 0 <= t < text_end and not (f <= t < ends[f]):
                tail.add(t)
    # a heuristic start strictly inside an exact function is not a function of its own: in compiled code nothing
    # calls into the middle of a function; in hand-written assembly `bl next; .word x` reads a PC-relative word
    # (nn::os::detail::UserExceptionHandler in sdk). Such addresses are kept as secondary entries, not functions.
    secondary = sorted(a for a in (bl | rel | tail) - set(starts) if containing(a) is not None)
    # the NSO header: word 0 is either `b <entry>` (rtld) or 0, word 1 the MOD0 offset; when word 0 is not a branch the
    # bytes up to the end of the MOD0 record (7 words) are data, and a relocated pointer to the module base (R at 0)
    # is not a function
    header = []
    if (words[0] & 0xfc000000) != 0x14000000:
        hdr_end = words[1] + 0x1c
        header = sorted(a for a in (bl | rel | tail) if a < hdr_end and a not in exact)
    allstarts = (set(starts) | bl | rel | tail) - set(secondary) - set(header)
    order = sorted(allstarts)
    rows = []
    for k, s in enumerate(order):
        src = ''.join(c for c, st in (('E', s in exact and 'E' in exact[s]), ('S', s in exact and 'S' in exact[s]),
                                      ('B', s in bl), ('R', s in rel), ('T', s in tail)) if st)
        nxt = order[k + 1] if k + 1 < len(order) else text_end
        if s == 0 and (words[0] & 0xfc000000) == 0x14000000:
            end, how = 4, 'nso-entry'             # `b start` followed by the MOD0 header words (data)
        elif s in ends:
            end, how = ends[s], 'exact'
        else:
            end, how = nxt, 'next'
        rows.append({'start': s, 'end': end, 'src': src, 'end_from': how, 'names': names.get(s, [])})

    # coverage of .text by exact functions
    covered = 0
    last = 0
    for s in starts:
        e = ends[s]
        if e > last:
            covered += e - max(s, last)
            last = e
    stats = {'text_size': text_end, 'functions': len(rows), 'exact_E': sum(1 for v in exact.values() if 'E' in v),
             'exact_S': sum(1 for v in exact.values() if 'S' in v),
             'exact_both': sum(1 for v in exact.values() if 'E' in v and 'S' in v),
             'E_S_end_disagree': len(disagree), 'E_S_disagree_examples': ['%x: E %x S %x' % x for x in disagree[:10]],
             'only_heuristic': sum(1 for r in rows if r['end_from'] not in ('exact', 'nso-entry')),
             'secondary_entries': len(secondary),
             'header_refs_dropped': ['%x' % a for a in header],
             'secondary_examples': ['%x in %x' % (a, containing(a)) for a in secondary[:10]],
             'bl_targets': len(bl), 'reloc_targets': len(rel), 'tail_targets': len(tail),
             'text_bytes_in_exact_functions': covered, 'named_by_dynsym': sum(1 for r in rows if r['names'])}
    return rows, stats


def rtti_nerves(mod, on_end=False):
    """{address: mangled name} of the execute functions of every class derived from al::Nerve, from RTTI. A typeinfo
    is a pointer to a __cxxabiv1 class_type_info vtable (+16, a symbol relocation), the name (a RELATIVE relocation to
    the type's mangled name) and, for single inheritance, the base typeinfo; a vtable is offset-to-top (no relocation),
    the typeinfo, then the virtual functions: slot 0 is al::Nerve's execute, slot 1 its executeOnEnd (run when the
    keeper leaves the state). on_end: also the classes' own executeOnEnd (not the default most classes share)."""
    syms = mod.symbols()
    rel, symrel = {}, {}
    for off, info, add in mod.relocs():
        kind = info & 0xffffffff
        if kind == 0x403:                                         # R_AARCH64_RELATIVE
            rel[off] = add
        elif kind in (0x101, 0x401):                              # R_AARCH64_ABS64, GLOB_DAT
            symrel[off] = syms[info >> 32][0]
    tinfo = {}
    for off, name in symrel.items():
        if name.startswith('_ZTVN10__cxxabiv1') and name.endswith('class_type_infoE') and off + 8 in rel:
            tinfo[off] = (mod.cstr(rel[off + 8]), rel.get(off + 16) if '_si_' in name else None)
    nerve = [t for t, (s, _) in tinfo.items() if s == 'N2al5NerveE']
    if len(nerve) != 1:
        return {}
    nerve = nerve[0]
    refs = collections.defaultdict(list)
    for off, p in rel.items():
        refs[p].append(off)
    out, ends = {}, []
    for t, (s, base) in tinfo.items():
        b, depth = base, 0
        while b is not None and b != nerve and b in tinfo and depth < 32:
            b, depth = tinfo[b][1], depth + 1
        if b != nerve:
            continue
        vts = [o - 8 for o in refs.get(t, []) if o - 8 not in rel and o - 8 not in symrel and o - 8 not in tinfo]
        fn = rel.get(vts[0] + 16) if len(vts) == 1 else None
        if fn is None:
            continue
        inner = s[1:-1] if s.startswith('N') and s.endswith('E') else s
        param = 'PNS_11NerveKeeperE' if inner.startswith('2al') else 'PN2al11NerveKeeperE'   # `al` is substitution S_
        out.setdefault(fn, '_ZNK' + inner + '7executeE' + param)
        if rel.get(vts[0] + 24) is not None:
            ends.append((rel[vts[0] + 24], '_ZNK' + inner + '12executeOnEndE' + param))
    if on_end and ends:
        shared = collections.Counter(a for a, _ in ends).most_common(1)[0][0]    # al::Nerve's own, which most keep
        for a, name in ends:
            if a != shared and a not in out:
                out.setdefault(a, name)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dump', default=T.DUMP)
    ap.add_argument('--out', default=T.build('functions'))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    summary = {}
    elfdir = os.path.join(a.dump, 'elf')
    for fn in sorted(os.listdir(elfdir)):
        name = fn[:-4]
        mod = Module(os.path.join(elfdir, fn))
        rows, stats = analyse(mod)
        rtti = rtti_nerves(mod, on_end=True) if name == 'main' else {}
        if rtti:
            on_end = sum(1 for n in rtti.values() if '12executeOnEndE' in n)
            rtti_exec = {a: n for a, n in rtti.items() if '12executeOnEndE' not in n}
            starts = {r['start'] for r in rows}
            same_sym = sum(1 for r in rows if r['start'] in rtti and rtti[r['start']] in r['names'])
            other_sym = sum(1 for r in rows if r['start'] in rtti and r['names'] and rtti[r['start']] not in r['names'])
            stats['rtti_nerves'] = {'execute_functions': len(rtti_exec), 'own_executeOnEnd': on_end,
                                    'not_a_listed_start': len(set(rtti) - starts),
                                    'same_as_symbol': same_sym, 'other_symbol': other_sym}
        summary[name] = stats
        with open(os.path.join(a.out, '%s_functions.tsv' % name), 'w', encoding='utf-8', newline='\n') as f:
            f.write('start\tend\tsize\tsrc\tend_from\tsymbol\trtti_name\n')
            for r in rows:
                f.write('%x\t%x\t%d\t%s\t%s\t%s\t%s\n' % (
                    r['start'], r['end'], r['end'] - r['start'], r['src'], r['end_from'], '|'.join(r['names']),
                    rtti.get(r['start'], '')))
        print('%-8s functions %6d  exact E %6d  S %6d  both %6d  E/S disagree %d  heuristic-only %d  secondary entries %d'
              '  .text covered %.2f%%' % (name, stats['functions'], stats['exact_E'], stats['exact_S'],
                                          stats['exact_both'], stats['E_S_end_disagree'], stats['only_heuristic'],
                                          stats['secondary_entries'],
                                          100.0 * stats['text_bytes_in_exact_functions'] / stats['text_size']))
        if 'rtti_nerves' in stats:
            print('         nerve states from RTTI:', json.dumps(stats['rtti_nerves']))
    with open(os.path.join(a.out, 'summary.json'), 'w') as f:
        json.dump(summary, f, indent=1)


if __name__ == '__main__':
    main()
