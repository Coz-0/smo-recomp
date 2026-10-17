"""Every shader stage of the game through the translator, offline: no need to play to the places that use them.

    python tests/check_all_shaders.py [--folders ShaderData,EffectData,SystemData,LayoutData] [--list]

1. Takes every NVN shader stage out of the game's shader containers in build/romfs (agl .sharcb, g3d .bfsha, the effect
   .ptcl, inside the SARC archives of the folders given) and out of main.elf / sdk.elf, and writes the distinct ones to
   build/shaders_all/*.mxs (derived from the game: under build/, ignored by git).
   A graphics stage is a code section (magic 0x12345678, program header at +0x30, instructions, constant data); its
   length comes from its own instructions (every program ends with a branch to itself). A compute stage has no header:
   it is found through its control section (magic 0x98761234; +0x6f8 code bytes, +0x6fc / +0x700 constant data size and
   offset, +0x714 NVN stage, +0x748 work group, +0x754 shared and local bytes). Programs that cannot be delimited are
   reported, not guessed (see stages_of).
2. Builds build/bin/mx_all.exe (tests/mx_all.c + runtime/mx_translate.c, clang 9) and runs it from build/bin on them:
   translation with the Vulkan back end's settings and compilation to SPIR-V with the renderer's compiler.
Exit status 0 when every stage translates and compiles.
"""
import argparse
import hashlib
import os
import struct
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROM = os.path.join(REPO, 'build', 'romfs')
OUT = os.path.join(REPO, 'build', 'shaders_all')
BIN = os.path.join(REPO, 'build', 'bin')
CLANG = os.path.join(os.environ.get('LLVM_ROOT', 'C:/llvm-9.0.0'), 'bin', 'clang.exe')
sys.path.insert(0, os.path.join(REPO, 'tools', 'dev'))
from sarc import yaz0  # noqa: E402

CTL, CODE = 0x98761234, 0x12345678


def sarc_files(d):
    e = '<' if d[6:8] == b'\xff\xfe' else '>'
    hdr, = struct.unpack_from(e + 'H', d, 4)
    data_off, = struct.unpack_from(e + 'I', d, 0xc)
    nodes, = struct.unpack_from(e + 'H', d, hdr + 6)
    names = hdr + 0xc + nodes * 0x10 + 8
    for i in range(nodes):
        _, attr, beg, end = struct.unpack_from(e + 'IIII', d, hdr + 0xc + i * 0x10)
        name = ''
        if attr & 0x01000000:
            o = names + (attr & 0xffff) * 4
            name = d[o:d.index(b'\0', o)].decode('utf-8', 'replace')
        yield name, d[data_off + beg:data_off + end]


BRA_M, BRA_V = 0xfff0000000000000, 0xe240000000000000
SPH_TO_MX = {1: 0, 5: 1, 4: 2, 2: 3, 3: 4}            # program header type -> MX_* stage


EXIT_M, EXIT_V = 0xfff0000000000000, 0xe300000000000000


def program_end(blob, start, limit, fallback=False):
    """Bytes from `start` (the first instruction group) to the end of the group holding the program's last instruction:
    the compiler ends a program with a branch to itself (BRA with offset -8), which appears nowhere else. Small programs
    may end with EXIT alone: with `fallback`, when there is no such branch before `limit` (the next code section), the
    group of the last EXIT is the end. None when neither is found."""
    q, last_exit = start, None
    while q + 32 <= limit:
        for k in (1, 2, 3):                            # word 0 of each group of four is scheduling information
            w, = struct.unpack_from('<Q', blob, q + 8 * k)
            if w & BRA_M == BRA_V and (w >> 20) & 0xffffff == 0xfffff8:
                return q + 32 - start
            if w & EXIT_M == EXIT_V:
                last_exit = q + 32 - start
        q += 32
    return last_exit if fallback else None


def stages_of(blob):
    """(stage, code bytes, constant data, work group, shared, local) of every program in a container, and the number that
    could not be delimited. Graphics: every code section (magic 0x12345678 at a 0x100 boundary, zero header words, the
    program header at +0x30), its length from its own instructions, its constant data the bytes from the next 0x100
    boundary after it to the next section (at most 64 KB; only jump tables are read from it at translation). Compute
    sections have no magic: in an agl archive (magic "BAHS") they follow each other from the code region (header word 7)
    in the order of their control sections (+0x714 stage 5), each checked to end within the size its control gives."""
    n4 = len(blob) // 4
    words = struct.unpack_from('<%dI' % n4, blob)
    codes = [i * 4 for i, w in enumerate(words) if w == CODE and (i * 4) % 0x100 == 0 and not any(words[i + 1:i + 12])]
    out, bad = [], 0
    for j, x in enumerate(codes):
        sph, = struct.unpack_from('<I', blob, x + 0x30)
        t = (sph >> 10) & 15
        limit = codes[j + 1] if j + 1 < len(codes) else len(blob)
        end = program_end(blob, x + 0x30 + 0x50, limit, fallback=True) if t in SPH_TO_MX else None
        if end is None:
            bad += 1
            continue
        code_len = 0x50 + end
        cstart = x + ((0x30 + code_len + 0xff) & ~0xff)
        out.append((SPH_TO_MX[t], blob[x + 0x30:x + 0x30 + code_len], blob[cstart:min(limit, cstart + 0x10000)], (0, 0, 0), 0, 0))
    if blob[:4] == b'BAHS':
        region, = struct.unpack_from('<I', blob, 0x1c)
        for c in [i * 4 for i, w in enumerate(words) if w == CTL]:
            if c < 0x60 or c + 0x760 > len(blob) or struct.unpack_from('<I', blob, c + 0x714)[0] != 5:
                continue
            # the stage's record holds its code offset (from the code region, header word 7) 0x60 bytes before the control
            # section; programs follow each other without padding, and a small one may end with EXIT alone
            pos = region + struct.unpack_from('<I', blob, c - 0x60)[0]
            size, cbsize, cboff = struct.unpack_from('<III', blob, c + 0x6f8)
            end = program_end(blob, pos, min(len(blob), pos + size), fallback=True) if pos + 32 <= len(blob) else None
            if not size or end is None or end > size:
                bad += 1
                continue
            out.append((5, blob[pos:pos + size], blob[pos + cboff:pos + cboff + cbsize], struct.unpack_from('<3I', blob, c + 0x748),
                        *struct.unpack_from('<2I', blob, c + 0x754)))
    else:
        starts = []
        i = blob.find(b'BNSH')
        while i >= 0:
            starts.append(i)
            i = blob.find(b'BNSH', i + 4)
        for c in [i * 4 for i, w in enumerate(words) if w == CTL and i * 4 + 0x760 <= len(blob) and words[i + 0x714 // 4] == 5]:
            # BNSH (nn::gfx): offsets are from the container's start, and a stage's control offset is stored as a u64 with
            # its code offset right after it; a compute code section starts with its first instruction group
            b = max((s for s in starts if s < c), default=None)
            size, cbsize, cboff = struct.unpack_from('<III', blob, c + 0x6f8)
            got = None
            if b is not None:
                pat, p = struct.pack('<Q', c - b), blob.find(struct.pack('<Q', c - b))
                while p >= 0 and got is None:
                    if p % 8 == 0 and p + 16 <= len(blob):
                        x = b + struct.unpack_from('<Q', blob, p + 8)[0]
                        end = program_end(blob, x, min(len(blob), x + size + 0x100), fallback=True) if x + 32 <= len(blob) else None
                        if end is not None and end <= size + 0x20:
                            got = x
                    p = blob.find(pat, p + 1)
            if got is None:
                bad += 1
                continue
            out.append((5, blob[got:got + size], blob[got + cboff:got + cboff + cbsize], struct.unpack_from('<3I', blob, c + 0x748),
                        *struct.unpack_from('<2I', blob, c + 0x754)))
    return out, bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--folders', default='ShaderData,EffectData,SystemData,LayoutData')
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--extract-only', action='store_true', help='write the stage files and the report, do not translate')
    a = ap.parse_args()
    os.makedirs(OUT, exist_ok=True)
    for n in os.listdir(OUT):
        if n.endswith('.mxs'):
            os.remove(os.path.join(OUT, n))
    seen, written, total, unpaired_all = set(), 0, 0, 0
    report = []

    def take(label, blob):
        nonlocal written, total, unpaired_all
        st, unpaired = stages_of(blob)
        if not st and not unpaired:
            return
        total += len(st)
        unpaired_all += unpaired
        new = 0
        for stage, code, cdata, local, smem, lmem in st:
            key = hashlib.sha1(bytes([stage]) + code + b'|' + cdata).digest()
            if key in seen:
                continue
            seen.add(key)
            new += 1
            written += 1
            with open(os.path.join(OUT, '%05d_%d.mxs' % (written, stage)), 'wb') as f:
                f.write(struct.pack('<4s8I', b'MXS1', stage, len(code), len(cdata), local[0], local[1], local[2], smem, lmem))
                f.write(code)
                f.write(cdata)
        report.append('%-60s %5d stages (%d new), %d not delimited' % (label[:60], len(st), new, unpaired))

    for folder in a.folders.split(','):
        base = os.path.join(ROM, folder)
        for root, _, files in os.walk(base):
            for n in sorted(files):
                if not n.endswith('.szs'):
                    continue
                path = os.path.join(root, n)
                d = yaz0(open(path, 'rb').read())
                if d[:4] != b'SARC':
                    continue
                todo = [(os.path.relpath(path, ROM), d)]
                while todo:                                    # archives inside archives (SystemData/AglResource.szs)
                    label, arc = todo.pop()
                    for name, data in sarc_files(arc):
                        inner = yaz0(data) if data[:4] == b'Yaz0' else data
                        if inner[:4] == b'SARC':
                            todo.append((label + ':' + name, inner))
                        elif struct.pack('<I', CTL) in inner:
                            take(label + ':' + name, inner)
    for mod in ('main', 'sdk'):
        p = os.path.join(REPO, 'build', 'dump', 'base', 'elf', mod + '.elf')
        if os.path.exists(p):
            take(mod + '.elf', open(p, 'rb').read())
    for line in report:
        print(line)
    print('stages found: %d, distinct: %d, not delimited: %d -> %s' % (total, written, unpaired_all, OUT))
    if a.extract_only:
        sys.exit(1 if unpaired_all else 0)
    exe = os.path.join(BIN, 'mx_all.exe')
    r = subprocess.run([CLANG, '-O1', '-w', '-I', os.path.join(REPO, 'runtime'), os.path.join(REPO, 'tests', 'mx_all.c'),
                        os.path.join(REPO, 'runtime', 'mx_translate.c'), '-o', exe], capture_output=True, text=True)
    if r.returncode:
        print((r.stdout + r.stderr)[-3000:])
        sys.exit('check_all_shaders: build of mx_all.exe failed')
    r = subprocess.run([exe, OUT] + (['--list'] if a.list else []), cwd=BIN)
    sys.exit(1 if r.returncode or unpaired_all else 0)


if __name__ == '__main__':
    main()
