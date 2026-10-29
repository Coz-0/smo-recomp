"""Targets of a BR that is a compiler jump table, read from the image.

Why: a BR the lifter cannot follow becomes `goto L_dispatch`, a switch over EVERY instruction of the function. For a
switch statement that is thousands of edges where the table has a handful: f_710061d5b0 (main) has 1 951 cases for a
table of 5, and clang spends 86 s on that one function (1 s with the 5 cases, code 43 -> 25 KB).

The shape clang gives a jump table, walking back from `br xD` (at most 16 instructions):
    xD = add xA, xB [, lsl/sxtw #s]      one of xA/xB is an entry, the other the anchor
    entry = ldrsw / ldr w / ldrh / ldrb (signed or not) [xT, xI, lsl #k]
    xT and the anchor = adrp (+ add #lo) or adr
Target i = anchor + (entry i << s). Entries are read until one is outside the function or misaligned: every real
entry is inside, so the list holds all of them (at worst a few more after the table, which are harmless cases). A BR
of any other shape returns None and keeps the full dispatch.

    targets(br_pc, entry, end, word, read) -> sorted distinct targets, or None
When the table's address was made far from the BR (before a loop, on another path), a forward constant propagation over
the function's branches gives it (as HWDER's const_dataflow): ADRP / ADR / ADD #imm / MOV between registers carry a
constant, a BL clobbers x0-x18 and x30 (AAPCS64), any other write forgets the register, and where paths meet a register
keeps its value only if every path agrees.
    loads_pointer(br_pc, entry, word) -> True when the BR's register was loaded from memory (a 64-bit LDR / LDP)

A BR whose target was loaded from memory is a call through a pointer (a vtable, a callback; in main 2 914 of the
3 663 BRs that are not readable tables): a pointer to the middle of a function would need a relocation, and main has
none (0 of 3 825 functions with a BR have an inner address in RELA, ADR or ADRP+ADD). It needs no dispatch over the
function's own instructions, only call_indirect.
word(addr) is the instruction word at addr, read(addr, fmt) a struct value of the image or None outside it (the
lifter's own readers: this module does not open the image).
"""

_LOOKBACK = 16 * 4
_LOADS = {0xb8a00800: ('<i', 2), 0xb8600800: ('<I', 2), 0x78a00800: ('<h', 1), 0x78600800: ('<H', 1),
          0x38a00800: ('<b', 0), 0x38600800: ('<B', 0)}          # register-offset loads, by opcode bits


def bits(w, hi, lo):
    return (w >> lo) & ((1 << (hi - lo + 1)) - 1)


def sext(v, n):
    return v - (1 << n) if v >> (n - 1) & 1 else v


def _writes(w, reg):
    """May w write general register reg? Exact for branches and loads/stores (where Rt is often only read: cbz, tbz,
    b.cond's condition, a store), conservative elsewhere (any instruction naming reg at bits 4-0)."""
    op0 = bits(w, 28, 25)
    if op0 in (10, 11):                                                     # branches, exceptions, system
        if (w & 0x7c000000) == 0x14000000:                                  # b / bl
            return bool(w >> 31) and reg == 30
        if (w & 0xfffffc1f) == 0xd63f0000:                                  # blr
            return reg == 30
        if (w & 0xfff00000) == 0xd5300000:                                  # mrs
            return bits(w, 4, 0) == reg
        return False                                                        # b.cond, cbz/cbnz, tbz/tbnz, br, ret, svc, hints
    if op0 in (4, 6, 12, 14):                                               # loads and stores
        if bits(w, 9, 5) == reg:
            if (w & 0x3a000000) == 0x28000000 and bits(w, 24, 23) in (1, 3):     # pair, pre/post-index
                return True
            if (w & 0x3b200000) == 0x38000000 and bits(w, 11, 10) in (1, 3):     # single, pre/post-index
                return True
        if (w & 0x3f000000) == 0x08000000:                                  # exclusive / ordered: status Ws, or Rt
            return bits(w, 20, 16) == reg or bits(w, 4, 0) == reg
        if bits(w, 26, 26):                                                 # SIMD & FP: no general register loaded
            return False
        if (w & 0x3a000000) == 0x28000000:                                  # pair
            return bool(bits(w, 22, 22)) and reg in (bits(w, 4, 0), bits(w, 14, 10))
        if (w & 0x3b000000) == 0x18000000:                                  # ldr (literal)
            return bits(w, 4, 0) == reg
        if (w & 0x3b200c00) == 0x38200000:                                  # atomic memory op (ldadd, swp...): loads Rt
            return bits(w, 4, 0) == reg
        if (w & 0x3b000000) == 0x38000000:                                  # single register
            return bits(w, 23, 22) != 0 and bits(w, 4, 0) == reg            # opc 00 = store
        return bits(w, 4, 0) == reg
    return bits(w, 4, 0) == reg


def _address(word, at, reg, lo):
    """The constant reg holds at `at`, made by adrp (+ add #lo12) or adr after `lo`; None if made otherwise."""
    add_lo = None
    pc = at - 4
    while pc >= lo:
        w = word(pc)
        if not _writes(w, reg):
            pc -= 4
            continue
        if (w & 0xffc00000) == 0x91000000 and bits(w, 9, 5) == reg and add_lo is None:
            add_lo = bits(w, 21, 10)                                       # add xR, xR, #lo12
        elif (w & 0x9f000000) == 0x90000000:                               # adrp
            return (pc & ~0xfff) + (sext((bits(w, 23, 5) << 2) | bits(w, 30, 29), 21) << 12) + (add_lo or 0)
        elif (w & 0x9f000000) == 0x10000000:                               # adr
            return pc + sext((bits(w, 23, 5) << 2) | bits(w, 30, 29), 21) + (add_lo or 0)
        else:
            return None
        pc -= 4
    return None


def _successors(word, pc, entry, end, tables):
    w = word(pc)
    if pc in tables:                                                        # a BR whose table is known
        return tables[pc]
    if (w & 0x7c000000) == 0x14000000 and not w >> 31:                     # b
        t = pc + sext(bits(w, 25, 0), 26) * 4
        return [t] if entry <= t < end else []
    if (w & 0xfffffc1f) in (0xd61f0000, 0xd65f0000):                        # br, ret
        return []
    t = None
    if (w & 0xff000010) == 0x54000000:                                      # b.cond
        t = pc + sext(bits(w, 23, 5), 19) * 4
    elif (w & 0x7e000000) == 0x34000000:                                    # cbz / cbnz
        t = pc + sext(bits(w, 23, 5), 19) * 4
    elif (w & 0x7e000000) == 0x36000000:                                    # tbz / tbnz
        t = pc + sext(bits(w, 18, 5), 14) * 4
    out = [pc + 4] if pc + 4 < end else []
    if t is not None and entry <= t < end:
        out.append(t)
    return out


def _step(word, pc, state):
    """The constants after the instruction at pc, from those before it."""
    w = word(pc)
    rd = bits(w, 4, 0)
    if (w & 0x9f000000) == 0x90000000:                                      # adrp
        return {**state, rd: (pc & ~0xfff) + (sext((bits(w, 23, 5) << 2) | bits(w, 30, 29), 21) << 12)}
    if (w & 0x9f000000) == 0x10000000:                                      # adr
        return {**state, rd: pc + sext((bits(w, 23, 5) << 2) | bits(w, 30, 29), 21)}
    if (w & 0xffc00000) == 0x91000000 and rd != 31:                         # add xd, xn, #imm12
        out = {k: v for k, v in state.items() if k != rd}
        if bits(w, 9, 5) in state:
            out[rd] = state[bits(w, 9, 5)] + bits(w, 21, 10)
        return out
    if (w & 0xffe0ffe0) == 0xaa0003e0:                                      # mov xd, xm (orr xd, xzr, xm)
        out = {k: v for k, v in state.items() if k != rd}
        if bits(w, 20, 16) in state:
            out[rd] = state[bits(w, 20, 16)]
        return out
    if (w & 0x7c000000) == 0x14000000 and w >> 31 or (w & 0xfffffc1f) == 0xd63f0000:   # bl, blr
        return {k: v for k, v in state.items() if 19 <= k <= 29}
    return {k: v for k, v in state.items() if not _writes(w, k)}


_FLOW = {}


def _constants(word, entry, end, tables):
    """{pc: {register: constant}} before each instruction reached from the entry (tables: {br_pc: targets})."""
    key = (entry, end, tuple(sorted(tables)), sum(len(t) for t in tables.values()))
    if key not in _FLOW:
        before = {entry: {}}
        work = [entry]
        while work:
            pc = work.pop()
            after = _step(word, pc, before[pc])
            for t in _successors(word, pc, entry, end, tables):
                if t not in before:
                    before[t] = after
                    work.append(t)
                    continue
                met = {k: v for k, v in before[t].items() if after.get(k) == v}
                if met != before[t]:
                    before[t] = met
                    work.append(t)
        _FLOW.clear()                                        # one function at a time: the lifter goes in order
        _FLOW[key] = before
    return _FLOW[key]


def _entries(read, base, fmt, esz, anchor, shift, entry, end):
    out = []
    for i in range(4096):
        v = read(base + (i << esz), fmt)
        if v is None:
            break
        t = (anchor + (v << shift)) % (1 << 64)
        if not entry <= t < end or t % 4:
            break
        out.append(t)
    return sorted(set(out)) or None


def _match(br_pc, entry, end, word, read, resolve):
    """The table of the BR at br_pc, the constants of its base and anchor given by resolve(at, reg); None if not one."""
    rd = bits(word(br_pc), 9, 5)
    lo = max(entry, br_pc - _LOOKBACK)
    pc = br_pc - 4
    while pc >= lo and not _writes(word(pc), rd):
        pc -= 4
    if pc < lo:
        return None
    w = word(pc)
    if (w & 0xff200000) == 0x8b000000 and bits(w, 23, 22) == 0:            # add (shifted register, lsl)
        shift = bits(w, 15, 10)
    elif (w & 0xffe00000) == 0x8b200000 and bits(w, 15, 13) in (2, 3, 6, 7):   # add (extended register)
        shift = bits(w, 12, 10)
    else:
        return None
    rn, rm = bits(w, 9, 5), bits(w, 20, 16)
    for e_reg, a_reg, sh in ((rm, rn, shift), (rn, rm, 0) if shift == 0 else (None, None, 0)):
        if e_reg is None:
            continue
        ld = pc - 4
        while ld >= lo and not _writes(word(ld), e_reg):
            ld -= 4
        if ld < lo or (word(ld) & 0xffe00c00) not in _LOADS:
            continue
        lw = word(ld)
        fmt, esz = _LOADS[lw & 0xffe00c00]
        if bits(lw, 12, 12) == 0 and esz:                                  # index not scaled by the entry size
            continue
        base, anchor = resolve(ld, bits(lw, 9, 5)), resolve(pc, a_reg)
        if base is None or anchor is None:
            continue
        return _entries(read, base, fmt, esz, anchor, sh, entry, end)
    return None


_FAR = {}


def _far_tables(entry, end, word, read):
    """{br_pc: targets} of the function's tables whose address was made further back, by the function's constant flow.

    The flow takes the edges of every table it knows (near ones first, then the far ones it finds, until nothing new);
    the far ones are then checked against the final flow. A BR left unknown that is not a call through a pointer
    could lead anywhere, so then no far table is trusted."""
    key = (entry, end)
    if key in _FAR:
        return _FAR[key]
    brs = [pc for pc in range(entry, end, 4) if (word(pc) & 0xfffffc1f) == 0xd61f0000]
    near = {pc: t for pc in brs for t in [_match(pc, entry, end, word, read, lambda at, r: _address(word, at, r, max(entry, pc - _LOOKBACK)))] if t}
    pointer = {pc for pc in brs if loads_pointer(pc, entry, word)}
    known, far = dict(near), {}
    for _ in range(16):
        flow = _constants(word, entry, end, known)
        found = {pc: t for pc in brs if pc not in near and pc not in pointer
                 for t in [_match(pc, entry, end, word, read, lambda at, r: flow.get(at, {}).get(r))] if t}
        if found == far:
            break
        far = found
        known = {**near, **far}
    unknown = [pc for pc in brs if pc not in known and pc not in pointer]
    result = {} if unknown or found != far else far
    _FAR.clear()                                             # one function at a time: the lifter goes in order
    _FAR[key] = result
    return result


def targets(br_pc, entry, end, word, read):
    t = _match(br_pc, entry, end, word, read, lambda at, r: _address(word, at, r, max(entry, br_pc - _LOOKBACK)))
    return t if t else _far_tables(entry, end, word, read).get(br_pc)


def loads_pointer(br_pc, entry, word):
    rd = bits(word(br_pc), 9, 5)
    lo = max(entry, br_pc - _LOOKBACK)
    pc = br_pc
    for _ in range(4):                                   # through register copies (mov xd, xs)
        pc -= 4
        while pc >= lo and not _writes(word(pc), rd):
            pc -= 4
        if pc < lo:
            return False
        w = word(pc)
        if (w & 0xffe0ffe0) == 0xaa0003e0 and bits(w, 4, 0) == rd:
            rd = bits(w, 20, 16)
            continue
        return ((w & 0xffc00000) == 0xf9400000            # ldr x, [x, #imm]
                or (w & 0xffe00c00) == 0xf8600800         # ldr x, [x, x{, lsl #3}]
                or (w & 0xffe00400) == 0xf8400400 and bits(w, 4, 0) == rd   # ldr x, [x, #imm]! / [x], #imm
                or (w & 0xffc00000) == 0xa9400000)        # ldp x, x, [x, #imm] (either register)
    return False
