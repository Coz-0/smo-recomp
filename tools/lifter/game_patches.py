"""Instructions of the game that the lifter replaces with C of its own (recomp.py lift_function), found by what they are.

    import game_patches; patches = game_patches.find(word, base, text_size)   # {address: C statement}

sead's PerspectiveProjection keeps tan(fovy / 2) at +0xac and the aspect ratio at +0xb0 (its near plane at +0x98). Every
read of them - the matrix, the screen-to-camera steps - is an `ldr s1` / `ldp s1, s2` / `ldp s2, s3` from [x0, #0xac]
right after `ldr s0, [x0, #0x98]` and `fadd s0, s0, s0` (or `ldr s1, [x0, #0x98]`, `fadd s1, s1, s1`, `fmov s0, #0.5`
for the matrix). Each becomes a call to rt_persp (runtime/game/smo_aspect.c) with the two values, which fits a 16:9
view to the window's shape, then the registers the instruction would have loaded. A site found other than the expected
number of times patches nothing (said on stdout).
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
    print('game_patches: %s' % ', '.join('%s x%d' % kv for kv in words.items()))
    return out


if __name__ == '__main__':
    import sys
    sys.path.insert(0, __file__.rsplit('/', 1)[0])
    import recomp
    for a, s in sorted(find(recomp.word, recomp.BASE, recomp.TEXT_SIZE).items()):
        print('%#x  %s' % (a, s))
