"""Generate the per-build tables the host runtime needs, from the user's own converted dump, into build/gen/.

    python tools/lifter/gen_tables.py

Writes (everything derived from the dump; build/ is ignored by git):
  image.h            image layout of main and sdk (segment ends, relocation tables, .dynsym, .init_array, entry
                     points), GAME_SYM_<name> addresses of the game functions the runtime calls by name, and the game
                     functions its fast path and test aids need (GAME_SYM_ of the sampler sort, GAME_VSYM_, GAME_FN_).
                     1.4.1's main keeps almost no symbols: those functions are found by what they are and what they do
                     (find_game_functions, find_sampler_sort): a class's vtable from its RTTI name, a game state's from
                     its nerve's, the strings a function loads, the functions it calls, the shape of its code. Each rule
                     must give exactly one function, or the entry is 0 and the runtime leaves that feature off.
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

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, 'tools', 'extract'))
sys.path.insert(0, os.path.join(REPO, 'tools'))
import functions as F                                     # noqa: E402  (the ELF reader of tools/extract)
import target as T                                        # noqa: E402

DUMP = os.path.join(T.DUMP, 'elf')
OUT = T.build('gen')
GUEST_BASE = 0x7100000000
SDK_BASE = 0x7500000000
# game functions the runtime calls by name (the console's own allocator lives in main for this game)
GAME_SYMS = ['malloc', 'free', 'calloc', 'realloc', 'aligned_alloc', 'nninitStartup', 'nnMain',
             '_ZdlPv']               # operator delete(void *): frees what the game's operator new (sead heaps) allocated
# Optional: runtime/game/smo_fast.c's sampler list sort of agl::driver::NVNMgr in host code. A main without these symbols
# (1.4.1's is stripped) gets them from find_sampler_sort, or 0 when that fails, and smo_fast.c then leaves the game's own
# code alone. Order: the sort, registerSampler_, the key comparison.
GAME_SYMS_OPTIONAL = ['_ZN4sead12PtrArrayImpl4sortEPFiPKvS2_E', '_ZN3agl6driver6NVNMgr16registerSampler_EiPK10NVNsamplerj',
                      '_ZN3agl6driver6NVNMgr18compareSamplerKey_EPKNS1_10SamplerKeyES4_']
# Where the virtual functions the rules start from sit in a class's vtable (vtable_slots: the functions after the
# offset-to-top and the typeinfo): an actor's init(const al::ActorInitInfo &) and a scene's init(const al::SceneInitInfo &).
ACTOR_INIT_SLOT, SCENE_INIT_SLOT = 1, 3
# al::LiveActor's pose keeper (its position, rotation and velocity), and the byte offsets in the keeper's vtable of the
# functions al::updatePoseTrans and al::setVelocityZero call through it (updatePoseTrans, getVelocityPtr)
POSE_KEEPER, POSE_TRANS_SLOT, POSE_VELOCITY_SLOT = 0x50, 0x80, 0x50
# The functions the rules start from (find_anchors): an actor's or a scene's init in its class's vtable (by the RTTI
# name), a game state's execute (by its nerve class's RTTI name: the function its execute slot branches to), or the one
# function that loads the address of a string. StageScene::init: a stage scene starts (test aids SMO_START_STAGE and
# SMO_STAGE_LIST, runtime/game/smo_nerve.c; GAME_VSYM_StageScene_init); the three states that change the stage find
# ChangeStageInfo's constructor and GameDataFunction::tryChangeNextStage.
ANCHORS = {
    'StageScene::init': ('scene', '10StageScene'),
    'HakoniwaSequence::exePlayStage': ('nerve', 'N12_GLOBAL__N_128HakoniwaSequenceNrvPlayStageE'),
    'ShineTowerBackDoor::exeEnter': ('nerve', 'N12_GLOBAL__N_126ShineTowerBackDoorNrvEnterE'),
    'ShineTowerRocket::exeWait': ('nerve', 'N12_GLOBAL__N_123ShineTowerRocketNrvWaitE'),
}
# The game functions the runtime calls (GAME_FN_<name>), in the order of image.h; how each is found: find_game_functions.
# ChangeStageInfo's constructor and GameDataFunction::tryChangeNextStage: a stage change of the game's own, as a door
# makes it (test aid SMO_STAGE_LIST, runtime/game/smo_nerve.c). al::updatePoseTrans and al::setVelocityZero: the player
# moved to a point, as PlayerPuppet::setTrans moves him (test aid, sequence step @warp).
GAME_FN_NAMES = ['ChangeStageInfo_ctor', 'tryChangeNextStage', 'updatePoseTrans', 'setVelocityZero']


def code_words(m, start, size):
    return list(struct.unpack_from('<%dI' % (size // 4), m.d, F.OFF + start))


def adrp_page(a, w):
    return (a & ~0xfff) + (F.sext(((w >> 29) & 3) | (((w >> 5) & 0x7ffff) << 2), 21) << 12)


def bl_targets(m, start, size):
    """The targets of a function's BL instructions, in order."""
    return [start + 4 * i + F.sext(w & 0x3ffffff, 26) * 4 for i, w in enumerate(code_words(m, start, size))
            if w & 0xfc000000 == 0x94000000]


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


def nerve_exe(m, rtti):
    """The function the execute slot (the first virtual function) of nerve class `rtti` branches to, or None."""
    slots = vtable_slots(m, rtti)
    if not slots or not slots[0]:
        return None
    for k in range(4):
        w = m.u32(slots[0] + 4 * k)
        if w & 0xfc000000 == 0x14000000:                 # B: the nerve's execute is a tail call of the host's exe...
            return slots[0] + 4 * k + F.sext(w & 0x3ffffff, 26) * 4
    return None


class Code:
    """main.elf's code, for finding game functions by what they do: every function with its exact bounds
    (build/functions/main_functions.tsv), the calls (BL) each makes, the addresses each forms (ADRP then ADD of the same
    register: strings) and the pointer slots each loads (ADRP then a 64-bit LDR)."""

    def __init__(self, m):
        self.m = m
        rows = csv.DictReader(open(T.build('functions', 'main_functions.tsv'), encoding='utf-8'), delimiter='\t')
        self.size = {int(r['start'], 16): int(r['size']) for r in rows}
        self.starts = sorted(self.size)
        self.rel = {off: add for off, info, add in m.relocs() if info & 0xffffffff == 0x403}
        w = np.array(m.words(), dtype=np.uint32)
        # the ADDs (immediate) and 64-bit LDRs based on the register an ADRP set, within 16 instructions and until the
        # register is written again or the code calls or branches away
        self.forms, self.slot_loads = {}, {}
        ws = w.tolist()
        for i in np.nonzero((w & 0x9f000000) == 0x90000000)[0].tolist():
            page, rd = adrp_page(i * 4, ws[i]), ws[i] & 31
            for k in range(i + 1, min(i + 17, len(ws))):
                v = ws[k]
                if (v >> 5) & 31 == rd:
                    if v & 0xffc00000 == 0x91000000:                        # ADD (immediate, 64-bit, no shift)
                        self.forms.setdefault(page + ((v >> 10) & 0xfff), []).append(k * 4)
                    elif v & 0xffc00000 == 0xf9400000:                      # LDR (64-bit, unsigned offset)
                        self.slot_loads.setdefault(page + ((v >> 10) & 0xfff) * 8, []).append(k * 4)
                if (v & 0x7c000000 == 0x14000000 or v & 0xfe9ffc1f == 0xd61f0000          # B, BL; BR, BLR, RET
                        or v & 31 == rd and not (v & 0x0a400000 == 0x08000000              # writes it (not a store,
                                                 or v & 0x7e000000 in (0x34000000, 0x36000000))):  # CBZ / TBZ)
                    break

    def owner(self, a):
        i = bisect.bisect_right(self.starts, a) - 1
        return self.starts[i] if i >= 0 and a < self.starts[i] + self.size[self.starts[i]] else None

    def calls(self, a):
        return bl_targets(self.m, a, self.size[a])

    def call_sites(self, a):
        """[(place, target)] of the BL instructions of function a."""
        return [(a + 4 * i, a + 4 * i + F.sext(w & 0x3ffffff, 26) * 4) for i, w in enumerate(code_words(self.m, a, self.size[a]))
                if w & 0xfc000000 == 0x94000000]

    def string_addrs(self, text):
        raw = b'\x00' + text.encode() + b'\x00'
        out, i = set(), self.m.d.find(raw)
        while i >= 0:
            out.add(i + 1 - F.OFF)
            i = self.m.d.find(raw, i + 1)
        return out

    def string_users(self, text):
        """The functions that form the address of the string `text`."""
        return {self.owner(p) for a in self.string_addrs(text) for p in self.forms.get(a, [])} - {None}

    def string_func(self, text):
        """The one function that forms the address of `text`, or None."""
        users = self.string_users(text)
        return users.pop() if len(users) == 1 else None

    def forms_in(self, func, text):
        """Places in func where the address of `text` is formed."""
        end = func + self.size[func]
        return sorted(p for a in self.string_addrs(text) for p in self.forms.get(a, []) if func <= p < end)

    def slot_users(self, slot):
        return {self.owner(p) for p in self.slot_loads.get(slot, [])} - {None}

    def pointer_loads(self, func):
        """[(place, function)] of the loads in func of a pointer slot that holds a function of main (RELATIVE)."""
        end = func + self.size[func]
        out = []
        for slot, places in self.slot_loads.items():
            target = self.rel.get(slot)
            if target in self.size:
                out += [(p, target) for p in places if func <= p < end]
        return sorted(out)


def one(found, what):
    """(address, how) when found holds exactly one function, else (0, why not)."""
    found = {a for a in found if a}
    return (found.pop(), what) if len(found) == 1 else (0, '%d functions are %s' % (len(found), what))


def find_anchors(code, anchors):
    """{name: (address, how)} of anchors {name: (kind, what)}: kind 'actor' / 'scene' (the init in the vtable of the
    class whose RTTI name is what), 'nerve' (the execute of that nerve class) or 'string' (the one function that loads
    the address of that string); (0, why not) where there is not exactly one."""
    f = {}
    for name, (kind, what) in anchors.items():
        if kind in ('actor', 'scene'):
            k = ACTOR_INIT_SLOT if kind == 'actor' else SCENE_INIT_SLOT
            slots = vtable_slots(code.m, what)
            a = slots[k] if slots and k < len(slots) else None
            f[name] = (a, 'slot %d of the vtable of %s' % (k, what)) if a else (0, 'no vtable of %s' % what)
        elif kind == 'nerve':
            a = nerve_exe(code.m, what)
            f[name] = (a, 'the execute of nerve %s' % what) if a else (0, 'no nerve %s' % what)
        else:
            a = code.string_func(what)
            f[name] = (a, 'the one function that loads the address of "%s"' % what) if a else (0, 'not exactly one function loads "%s"' % what)
    return f


def find_game_functions(code):
    """{name: (address, how)} of the ANCHORS and GAME_FN_NAMES, each found by its rule; (0, why not) where the rule
    does not give one function."""
    f = find_anchors(code, ANCHORS)
    A = {name: a for name, (a, _) in f.items()}

    def calls(name):
        return set(code.calls(A[name])) if A.get(name) else set()

    # a stage change of the game's own: the play state of the stage sequence, the Odyssey's back door and its rocket
    # each make a ChangeStageInfo (its constructor sets the wipe "WipeCircle") and change the stage with it
    states = ['HakoniwaSequence::exePlayStage', 'ShineTowerBackDoor::exeEnter', 'ShineTowerRocket::exeWait']
    shared = set.intersection(*[calls(s) for s in states])
    ctor = one({a for a in shared if code.forms_in(a, 'WipeCircle')},
               'called by the play state, the back door and the rocket and loading "WipeCircle"')
    f['ChangeStageInfo_ctor'] = ctor
    later = set()
    if ctor[0]:
        for a in shared - {ctor[0]}:
            seqs = [code.calls(A[s]) for s in states]
            if all(ctor[0] in s and a in s[s.index(ctor[0]) + 1:] for s in seqs):
                later.add(a)
    f['tryChangeNextStage'] = one(later, 'called by all three after the constructor')
    # moving an actor: the 4-instruction wrappers of its pose keeper's virtual functions
    f['updatePoseTrans'] = one(set(pose_wrappers(code)), 'loading the pose keeper (+0x%x) and branching to its +0x%x'
                               % (POSE_KEEPER, POSE_TRANS_SLOT))
    f['setVelocityZero'] = one(set(velocity_zeroers(code)), 'zeroing the vector the pose keeper\'s +0x%x returns' % POSE_VELOCITY_SLOT)
    return f


def pose_wrappers(code):
    """Functions `ldr x0, [x0, #POSE_KEEPER]; ldr x8, [x0]; ldr xN, [x8, #POSE_TRANS_SLOT]; br xN`."""
    out = []
    for a, s in code.size.items():
        if s != 16:
            continue
        w = code_words(code.m, a, 16)
        if (w[0] & 0xffc003ff == 0xf9400000 and ((w[0] >> 10) & 0xfff) * 8 == POSE_KEEPER and w[1] == 0xf9400008
                and w[2] & 0xffc003e0 == 0xf9400100 and ((w[2] >> 10) & 0xfff) * 8 == POSE_TRANS_SLOT
                and w[3] & 0xfffffc1f == 0xd61f0000 and (w[3] >> 5) & 31 == w[2] & 31):
            out.append(a)
    return out


def velocity_zeroers(code):
    """Functions that load the pose keeper (x0 = [x0 + POSE_KEEPER]), call its function at POSE_VELOCITY_SLOT and store
    zero to the 12 bytes it returns: `ldr x8, [x0]; ldr x8, [x8, #slot]; blr x8; str xzr, [x0]; str wzr, [x0, #8]`."""
    keeper = 0xf9400000 | ((POSE_KEEPER // 8) << 10)                       # ldr x0, [x0, #POSE_KEEPER]
    slot = 0xf9400108 | ((POSE_VELOCITY_SLOT // 8) << 10)                   # ldr x8, [x8, #POSE_VELOCITY_SLOT]
    want = [keeper, 0xf9400008, slot, 0xd63f0100, 0xf900001f, 0xb900081f]
    out = []
    for a, s in code.size.items():
        if 24 <= s <= 64:
            w = code_words(code.m, a, s)
            if any(w[i:i + 6] == want for i in range(len(w) - 5)):
                out.append(a)
    return out


def find_sampler_sort(code):
    """GAME_SYMS_OPTIONAL from what they do: the NVN loader stores the function the driver gives for
    "nvnSamplerPoolRegisterSampler" in a global, through a pointer slot it loads right after it asks for it;
    registerSampler_ is the one other function that loads that slot and also loads a pointer to a function of main (the
    key comparison), and the sort is the first function it calls after that load, which must call nothing directly
    (it calls the comparison through its pointer). The sort's algorithm itself is checked at run time with
    SMO_FAST_CHECK=1 (runtime/game/smo_fast.c). Returns ({name: address}, None) or ({}, why not)."""
    sort_n, reg_n, cmp_n = GAME_SYMS_OPTIONAL
    loader = code.string_func('nvnSamplerPoolRegisterSampler')
    if not loader:
        return {}, 'not exactly one function loads "nvnSamplerPoolRegisterSampler"'
    at = code.forms_in(loader, 'nvnSamplerPoolRegisterSampler')[0]
    near = sorted((p, s) for s, places in code.slot_loads.items() for p in places if at < p < at + 40)
    if not near:
        return {}, 'the NVN loader stores "nvnSamplerPoolRegisterSampler" nowhere it loads'
    found = set()
    for f in code.slot_users(near[0][1]) - {loader}:
        sites = code.call_sites(f)
        for place, cmp_ in code.pointer_loads(f):
            later = [t for p, t in sites if p > place]
            if later:
                found.add((f, cmp_, later[0]))
    if len(found) != 1:
        return {}, '%d functions using the sampler pool\'s registration load a function pointer and call' % len(found)
    reg, cmp_, sort = found.pop()
    words = code_words(code.m, sort, code.size.get(sort, 0))
    if not words or any(w & 0xfc000000 == 0x94000000 for w in words) or not any(w & 0xfffffc1f == 0xd63f0000 for w in words):
        return {}, 'the function called after the load does not call only through pointers'
    return {sort_n: sort, reg_n: reg, cmp_n: cmp_}, None


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
    code = Code(main_m)
    missing = [n for n in GAME_SYMS_OPTIONAL if n not in defined]
    matched, why = find_sampler_sort(code) if missing else ({}, None)
    for n in GAME_SYMS_OPTIONAL:
        if n in defined:
            h.append('#define GAME_SYM_%s (GUEST_BASE + 0x%xULL)' % (n, defined[n]))
        elif n in matched:
            h.append('#define GAME_SYM_%s (GUEST_BASE + 0x%xULL)      /* found by what it does (find_sampler_sort) */'
                     % (n, matched[n]))
        else:
            h.append('#define GAME_SYM_%s 0ULL      /* not in this main.elf */' % n)
    found = find_game_functions(code)
    a, how = found['StageScene::init']
    vsyms = ['StageScene_init %s' % ('0x%x (%s)' % (a, how) if a else 'not found: ' + how)]
    h.append('#define GAME_VSYM_StageScene_init %s      /* %s */' % ('(GUEST_BASE + 0x%xULL)' % a if a else '0ULL', how))
    fns = []
    for short in GAME_FN_NAMES:
        a, how = found[short]
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
    found_data = []
    for n in imports:
        if n.startswith(('_ZN2nn4util', '_ZN2nn3hid6NpadId', '_ZNK2nn4util')) and n in sdk_objs and 0 < sdk_objs[n][1] <= 1 << 20:
            v, s = sdk_objs[n]
            found_data.append((n, sdk_m.d[0x1000 + v:0x1000 + v + s]))
    c = ['/* generated by tools/lifter/gen_tables.py from the real sdk module -- do not edit */', '#include "hostrt.h"', '']
    for k, (n, data) in enumerate(found_data):
        c.append('static const unsigned char d%d[] = {%s};' % (k, ','.join(str(x) for x in data)))
    c += ['', 'const SdkData sdk_data[] = {'] + ['  { "%s", d%d, %d },' % (n, k, len(data)) for k, (n, data) in enumerate(found_data)]
    if not found_data:
        c.append('  { 0, 0, 0 },')
    c += ['};', 'const int sdk_data_count = %d;' % len(found_data), '']
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
    print('imports: %d   sdk data symbols: %d   game symbols: %s' % (len(imports), len(found_data), ', '.join(GAME_SYMS)))
    if matched:
        print('not in this main.elf, found by what they do: %s' % ', '.join(
            '%s 0x%x' % (n, matched[n]) for n in GAME_SYMS_OPTIONAL if n in matched))
    elif missing:
        print('not in this main.elf (their fast path stays off: %s): %s' % (why, ', '.join(missing)))
    print('virtual functions: %s' % '; '.join(vsyms))
    print('game functions: %s' % '; '.join(fns))
    print('wrote', OUT)


if __name__ == '__main__':
    main()
