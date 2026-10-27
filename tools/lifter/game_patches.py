"""Instructions of the game that the lifter replaces with C of its own (recomp.py lift_function), found by what they are.

    import game_patches; patches = game_patches.find(word, base, text_size)   # {address: C statement}

sead's PerspectiveProjection keeps tan(fovy / 2) at +0xac and the aspect ratio at +0xb0 (its near plane at +0x98). Every
read of them - the matrix, the screen-to-camera steps - is an `ldr s1` / `ldp s1, s2` / `ldp s2, s3` from [x0, #0xac]
right after `ldr s0, [x0, #0x98]` and `fadd s0, s0, s0` (or `ldr s1, [x0, #0x98]`, `fadd s1, s1, s1`, `fmov s0, #0.5`
for the matrix). Each becomes a call to rt_persp (runtime/game/smo_aspect.c) with the two values, which fits a 16:9
view to the window's shape, then the registers the instruction would have loaded. A site found other than the expected
number of times patches nothing (said on stdout).

Functions of the game the runtime does in host code: a call put before the first instruction of the function, found
by its first words (an ADRP's page left out, which moves with the code); a nonzero answer returns from the function,
zero runs the lifted code. sead's two Yaz0 decoders (runtime/game/smo_yaz0.c): SZSDecompressor::streamDecomp, its
first 24 words, and the whole-buffer decoder, all of it. Such a patch is a tuple ('before', C), the instruction's own
C after it.
"""
LDR_S0_98, FADD_S0 = 0xbd409800, 0x1e202800
LDR_S1_98, FADD_S1, FMOV_S0_HALF = 0xbd409801, 0x1e212821, 0x1e2c1000
# (words before the load, the load, registers it loads: tan, aspect (None: tan only), expected count)
SITES = [
    ('persp_unproject_y', [LDR_S0_98, FADD_S0], 0xbd40ac01, (1, None), 3),
    ('persp_unproject_x', [LDR_S0_98, FADD_S0], 0x2d558801, (1, 2), 2),
    ('persp_matrix', [LDR_S1_98, FADD_S1, FMOV_S0_HALF], 0x2d558c02, (2, 3), 1),
    ('persp_project', [LDR_S0_98, FADD_S0], 0x2d558c02, (2, 3), 1),
]

# (name, the function's first words, the runtime's handler: int handler(Ctx *))
FUNCS = [
    ('yaz0_stream', [0x3940680a, 0x3400088a, 0xaa1f03ed, 0x5100044b, 0x12800008, 0xf0002f4c, 0x912cf18c, 0xaa0d03e9,
                     0x72001d5f, 0x540005a0, 0x2a0a03ee, 0x5100054a, 0x12001dcd, 0x510035ad, 0x71000dbf, 0x3900680a,
                     0x54000108, 0xb8ad798d, 0x8b0c01ad, 0xd61f01a0, 0x3869682d, 0x7100c1bf, 0x54000280, 0x140000cc],
     'h_yaz0_stream'),
    ('yaz0_whole', [0xb9400425, 0x5ac008a4, 0x2a0403e2, 0x91004021, 0x52800005, 0x5282000e, 0x510005ce, 0x53017ca5,
                    0x35000065, 0x38401426, 0x52801005, 0x6a0500df, 0x54000261, 0x38401423, 0x38401427, 0x0b0320e3,
                    0x530c7c69, 0x0a0e0063, 0x11000463, 0x11000927, 0x35000069, 0x38401427, 0x110048e7, 0x4b070084,
                    0x4b0303ea, 0x386ac80c, 0x710004e7, 0x3800140c, 0x54ffffa1, 0x35fffd44, 0x14000005, 0x3840142c,
                    0x71000484, 0x3800140c, 0x54fffca1, 0x2a0203e0],
     'h_yaz0_whole'),
]
ADRP_MASK, ADRP = 0x9f000000, 0x90000000


def same_word(have, want):
    if want & ADRP_MASK == ADRP:
        return have & 0x9f00001f == want & 0x9f00001f
    return have == want


def find_funcs(word, base, text_size):
    """{name: address} of each FUNCS entry whose words are found exactly once."""
    out = {}
    for name, words, _ in FUNCS:
        hits = [base + off for off in range(0, text_size - 4 * len(words), 4)
                if word(base + off) == words[0] and all(same_word(word(base + off + 4 * i), w) for i, w in enumerate(words))]
        if len(hits) != 1:
            print('game_patches: %s found %d times, expected 1: left to the lifted code' % (name, len(hits)))
            continue
        out[name] = hits[0]
    return out


def stmt(name, a, regs):
    t, k = regs
    keep = ' vzero(&c->v[%d]); c->v[%d].w[0] = k;' % (k, k) if k is not None else ''
    return ('{ extern void rt_persp(uint64_t, uint32_t *, uint32_t *); uint32_t t = rd32(c->x[0] + 0xac), k = rd32(c->x[0] + 0xb0); '
            'rt_persp(c->x[0], &t, &k); vzero(&c->v[%d]); c->v[%d].w[0] = t;%s }  /* %#x: %s (tan(fovy / 2), aspect) */'
            % (t, t, keep, a, name))


def find(word, base, text_size):
    """{address: C statement} for every site found exactly as often as expected."""
    words = {}
    out = {}
    for name, before, load, regs, count in SITES:
        hits = []
        for off in range(4 * len(before), text_size, 4):
            a = base + off
            if word(a) != load:
                continue
            if all(word(a - 4 * (len(before) - i)) == w for i, w in enumerate(before)):
                hits.append(a)
        if len(hits) != count:
            print('game_patches: %s found %d times, expected %d: not patched' % (name, len(hits), count))
            continue
        for a in hits:
            out[a] = stmt(name, a, regs)
        words[name] = len(hits)
    for name, a in find_funcs(word, base, text_size).items():
        h = next(f[2] for f in FUNCS if f[0] == name)
        out[a] = ('before', '{ extern int %s(Ctx *); if (%s(c)) return; }  /* %#x: %s in host code */' % (h, h, a, name))
        words[name] = 1
    print('game_patches: %s' % ', '.join('%s x%d' % kv for kv in words.items()))
    return out


if __name__ == '__main__':
    import sys
    sys.path.insert(0, __file__.rsplit('/', 1)[0])
    import recomp
    for a, s in sorted(find(recomp.word, recomp.BASE, recomp.TEXT_SIZE).items()):
        print('%#x  %s' % (a, s if isinstance(s, str) else s[1]))
