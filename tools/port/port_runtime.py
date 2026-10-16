"""One-time port of the runtime copied from ssbu-recomp (GPL-3.0-or-later) to SMO. Idempotent: an edit already applied
is skipped; an edit whose original text is not found exactly the expected number of times stops the script.

    python tools/port/port_runtime.py

What it changes, file by file, is listed in EDITS below; every 13.0.4 address in live code is either replaced by a
generated constant (build/gen/image.h) or disabled with a note (SMO_UNPORTED) when it belongs to a Smash-only
diagnostic. Afterwards: SSBU_ environment variables become SMO_, ssbu_* file / window / DLL names become smo_*.
"""
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
RT = os.path.join(REPO, 'runtime')

EDITS = {
    'hostrt.h': [
        ('#define GUEST_BASE 0x7100000000ULL\n',
         '#define GUEST_BASE 0x7100000000ULL\n#include "image.h"               /* generated per build: tools/lifter/gen_tables.py */\n'
         '/* SMO port: code that used a Super Smash Bros. Ultimate 13.0.4 address for a Smash-only diagnostic or test and has no\n'
         ' * SMO equivalent yet is disabled through this constant (never used for game behaviour). */\n#define SMO_UNPORTED 0\n', 1),
    ],
    'hostrt.c': [
        ('#define FILE_OFF 0x1000ULL\n#define FILE_IMAGE 0x52a8570ULL\n#define IMAGE_SIZE 0x7500000ULL\n'
         '#define GUEST_RO_END 0x4f16000ULL        /* start of the data segment (third PT_LOAD of main.elf): text and read-only data end here */\n'
         '#define RELA 0x39c8070ULL\n#define RELASZ 0x7ad0d8ULL\n#define JMPREL 0x4175148ULL\n#define PLTRELSZ 0xc600ULL\n',
         '/* SMO port: the image constants come from build/gen/image.h (generated from the dump) */\n#define FILE_OFF 0x1000ULL\n'
         '#define FILE_IMAGE GUEST_FILE_IMAGE\n#define IMAGE_SIZE GUEST_IMAGE_SIZE\n#define RELA GUEST_RELA\n#define RELASZ GUEST_RELASZ\n'
         '#define JMPREL GUEST_JMPREL\n#define PLTRELSZ GUEST_PLTRELSZ\n', 1),
        ('rd8(GUEST_BASE + 0x4197610ULL + (uint64_t)sym * 24 + 4)', 'rd8(GUEST_BASE + GUEST_DYNSYM + (uint64_t)sym * 24 + 4)', 1),
        ('addr < 0x7500cfb000ULL', 'addr < 0x7500000000ULL + SDK_IMAGE_SIZE_GEN', 1),
    ],
    'boot.c': [
        ('#define NNINIT_STARTUP (GUEST_BASE + 0x2c55e0ULL)\n#define NN_MAIN (GUEST_BASE + 0x2c5620ULL)\n'
         '#define INIT_ARRAY (GUEST_BASE + 0x52a7be0ULL)\n#define INIT_ARRAY_SIZE 0x990ULL\n',
         '/* SMO port: entry points and .init_array from build/gen/image.h */\n#define NNINIT_STARTUP GAME_SYM_nninitStartup\n'
         '#define NN_MAIN GAME_SYM_nnMain\n#define INIT_ARRAY GUEST_INIT_ARRAY\n#define INIT_ARRAY_SIZE GUEST_INIT_ARRAY_SIZE\n', 1),
        ('"main.npdm");', '"..\\\\exefs\\\\main.npdm");   /* SMO port: build/dump/base/{elf,exefs} */', 1),
        ('g_elf = argc > 1 ? argv[1] : "..\\\\binaries\\\\13.0.4\\\\main.elf";',
         'g_elf = argc > 1 ? argv[1] : "..\\\\dump\\\\base\\\\elf\\\\main.elf";   /* SMO port: run from build/bin */', 1),
        ('            {   /* the global std::map at 0x710593c3c0 the main thread walks: header and first nodes */\n',
         '            if (SMO_UNPORTED) {   /* (SSBU 13.0.4 diagnostic) the global std::map at 0x710593c3c0 the main thread walks */\n', 1),
        ('uint64_t m = 0x710593c3c0ULL;', 'uint64_t m = 0 /* SSBU 13.0.4 address, no SMO equivalent */;', 1),
    ],
    'sdklift.h': [
        ('#define SDK_IMAGE_SIZE 0xcfb000ULL', '#define SDK_IMAGE_SIZE SDK_IMAGE_SIZE_GEN     /* SMO port: build/gen/image.h */', 1),
    ],
    'sdklift_rt.c': [
        ('"..\\\\binaries\\\\13.0.4\\\\main.elf"', '"..\\\\dump\\\\base\\\\elf\\\\main.elf"', 1),
    ],
    'libcxx.c': [
        ('#define GAME_MEMALIGN 0x710392dce0ULL', '#define GAME_MEMALIGN GAME_SYM_aligned_alloc   /* SMO port: the game\'s own allocator, by symbol */', 1),
        ('#define GAME_FREE 0x710392e590ULL', '#define GAME_FREE GAME_SYM_free', 1),
    ],
    'nncxxlib.c': [
        ('call_guest(c, 0x710392e590ULL);', 'call_guest(c, GAME_SYM_free);', 1),
    ],
    'nnmovie_mf.c': [
        ('#define GAME_FREE 0x710392e590ULL', '#define GAME_FREE GAME_SYM_free', 1),
        ('if (g_test_resync_at && d->is_video', 'if (SMO_UNPORTED && g_test_resync_at && d->is_video', 1),
        ('uint8_t *req = (uint8_t *)(uintptr_t)0x7107446040ULL;', 'uint8_t *req = (uint8_t *)(uintptr_t)0 /* SSBU 13.0.4 address */;', 1),
    ],
    'nnmisc.c': [
        ('#define LUAD_RAWRUNPROTECTED 0x71038faf00ull\n#define LUAD_THROW 0x71038fae10ull\n',
         '/* SMO port: SMO has no Lua; this self-test used SSBU 13.0.4 functions and is disabled (SMO_UNPORTED) */\n'
         '#define LUAD_RAWRUNPROTECTED 0ull\n#define LUAD_THROW 0ull\n', 1),
        ('if (getenv("SSBU_MISC_SELFTEST")) misc_selftest();', 'if (SMO_UNPORTED && getenv("SSBU_MISC_SELFTEST")) misc_selftest();', 1),
        ('(void *)(uintptr_t)(0x7104f16000ull - 1)', '(void *)(uintptr_t)(GUEST_BASE + GUEST_RO_END - 1)', 1),
        ('IsBadWritePtr((void *)(uintptr_t)0x7104f16000ull, 1)', 'IsBadWritePtr((void *)(uintptr_t)(GUEST_BASE + GUEST_RO_END), 1)', 1),
        ('IsBadWritePtr((void *)(uintptr_t)0x7100000000ull, 1)', 'IsBadWritePtr((void *)(uintptr_t)GUEST_BASE, 1)', 1),
        ('IsBadReadPtr((void *)(uintptr_t)0x7100000000ull, 16)', 'IsBadReadPtr((void *)(uintptr_t)GUEST_BASE, 16)', 1),
    ],
    'nnsdk.c': [
        ('return on && (lr == 0x7100047d20ULL || lr == 0x7100138534ULL || lr == 0x710366d1f0ULL || lr == 0x710366d388ULL);',
         '(void)lr;\n    /* SMO port: the SSBU 13.0.4 seed call sites (0x7100047d20, 0x7100138534, 0x710366d1f0, 0x710366d388) do not exist in SMO;\n'
         '     * SMO\'s own seed sites are not found yet, so SMO_FIXED_SEED has no effect (SMO_UNPORTED). */\n    return SMO_UNPORTED && on;', 1),
    ],
    'nnsdk2.c': [
        ('call_guest(c, 0x71038697e0ULL);', 'if (SMO_UNPORTED) call_guest(c, 0 /* SSBU 13.0.4 address; SMO has no C++ exceptions */);', 1),
        ('bad += nnro_selftest(c);', '/* SMO port: no NRO script modules in SMO (nnro_selftest dropped) */', 1),
        ('int nnro_selftest(Ctx *c);\n', '', 1),
    ],
    'os_threads.c': [
        ('c->import_from != 0x71036f2814ULL', 'c->import_from != 0 /* SSBU 13.0.4 address */ || !SMO_UNPORTED', 1),
    ],
    'nvn_vk_m4.c': [
        ('uint64_t t = g64(0x71052a32d8ull);', 'uint64_t t = SMO_UNPORTED ? g64(0) : 0;   /* SSBU 13.0.4 table address: no SMO equivalent yet */', 1),
        ('uint64_t v = g64(0x7104470aa8ull + (uint64_t)i * 8);', 'uint64_t v = 0; (void)i;   /* SSBU 13.0.4 table address */', 1),
    ],
    'nvn_gl.c': [
        ('c->x[30] - 0x71000382a0ull < 2884', 'SMO_UNPORTED /* SSBU 13.0.4 caller range 0x71000382a0 */', 1),
    ],
    'nncxxrt.c': [
        ('#include "cxxrt_selftest.h"', '/* SMO port: cxxrt_selftest.h (SSBU 13.0.4 addresses and exception tables) is not built */\n'
                                        'static void cxxrt_selftest(void) { printf("C++ runtime self-test: not available for SMO yet\\n"); }', 1),
    ],
}


def renamed_text(s):
    t = re.sub(r'\bSSBU_(?=[A-Z0-9])', 'SMO_', s)
    t = re.sub(r'\bssbu_(?=[a-z0-9])', 'smo_', t)
    return t.replace('"ssbu-native"', '"smo-native"')


def apply():
    total = 0
    for name, edits in EDITS.items():
        p = os.path.join(RT, name)
        s = open(p, encoding='latin-1', newline='').read()
        nl = '\r\n' if '\r\n' in s else '\n'
        for old, new, count in edits:
            o, n = old.replace('\n', nl), new.replace('\n', nl)
            if n in s or renamed_text(n) in s:
                continue                                  # already applied (possibly with the names renamed since)
            k = s.count(o)
            if k != count:
                raise SystemExit('%s: expected %d match(es) of %r, found %d' % (name, count, old[:80], k))
            s = s.replace(o, n)
            total += 1
        open(p, 'w', encoding='latin-1', newline='').write(s)
    # names: environment variables and the program's own file / window / DLL names
    renamed = 0
    for name in sorted(os.listdir(RT)):
        if not name.endswith(('.c', '.h', '.inc', '.bat')):
            continue
        p = os.path.join(RT, name)
        s = open(p, encoding='latin-1', newline='').read()
        t = renamed_text(s)
        if t != s:
            renamed += 1
            open(p, 'w', encoding='latin-1', newline='').write(t)
    print('edits applied: %d   files with renamed names: %d' % (total, renamed))


if __name__ == '__main__':
    apply()
