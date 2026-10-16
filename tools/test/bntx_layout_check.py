"""Check the runtime's texture layout (runtime/tex_decode.c tex_layout) against every texture SMO ships.

    python tools/test/bntx_layout_check.py [--limit N] [--show 12]

Reference: the BNTX texture containers in the RomFS (inside Yaz0-compressed SARC / BFRES archives, found by their
"BNTX" header). Each texture record (BRTI) holds what Nintendo's tools computed with NVN's layout rules: image size,
alignment, block height (texture layout word, bits 0..2) and the offset of every mip level. For each texture the same
parameters go through tex_layout and every field is compared. Formats are mapped only where the block size is certain
(R8, RG8, RGB565, RGBA8, BGRA8, BC1..BC7, all ASTC footprints); other formats are counted as unmapped, not guessed.
Needs build/test/layout_probe.dll (built here with clang from tools/test/layout_probe.c + runtime/tex_decode.c).
"""
import argparse
import collections
import ctypes
import os
import struct
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ROMFS = os.path.join(REPO, 'build', 'romfs')
OUT = os.path.join(REPO, 'build', 'test')
CLANG = os.path.join(os.environ.get('LLVM_ROOT', 'C:/llvm-9.0.0'), 'bin', 'clang.exe')

# BNTX surface type (high byte of the format word) -> a NVN format with the same block size and footprint
NVN_FOR_TYPE = {0x02: 0x01, 0x07: 0x3C, 0x09: 0x0D, 0x0B: 0x25, 0x0C: 0x25,
                0x1A: 0x42, 0x1B: 0x43, 0x1C: 0x44, 0x1D: 0x49, 0x1E: 0x4B, 0x1F: 0x4F, 0x20: 0x4D}
NVN_FOR_TYPE.update({0x2D + i: 0x79 + i for i in range(14)})             # ASTC 4x4 .. 12x12


class ProbeOut(ctypes.Structure):
    _fields_ = [('size', ctypes.c_int64), ('data_size', ctypes.c_int64), ('alignment', ctypes.c_int64),
                ('layer_stride', ctypes.c_int64), ('bh_log2', ctypes.c_int32), ('bd_log2', ctypes.c_int32),
                ('levels', ctypes.c_int32), ('unknown_format', ctypes.c_int32), ('lvl_offset', ctypes.c_int64 * 16)]


def build_probe():
    os.makedirs(OUT, exist_ok=True)
    dll = os.path.join(OUT, 'layout_probe.dll')
    srcs = [os.path.join(REPO, 'tools', 'test', 'layout_probe.c'), os.path.join(REPO, 'runtime', 'tex_decode.c')]
    if not os.path.exists(dll) or os.path.getmtime(dll) < max(os.path.getmtime(s) for s in srcs):
        r = subprocess.run([CLANG, '-shared', '-O2', '-w', '-I', os.path.join(REPO, 'runtime')] + srcs + ['-o', dll],
                           capture_output=True, text=True)
        if r.returncode:
            raise SystemExit(r.stdout + r.stderr)
    lib = ctypes.CDLL(dll)
    lib.yaz0_decompress.restype = ctypes.c_int64
    lib.yaz0_decompress.argtypes = [ctypes.c_char_p, ctypes.c_int64, ctypes.c_char_p, ctypes.c_int64]
    lib.probe_layout.argtypes = [ctypes.c_int] * 7 + [ctypes.POINTER(ProbeOut)]
    return lib


def textures(blob):
    """Yield (name, BRTI fields) for every BNTX found in blob."""
    pos = 0
    while True:
        b = blob.find(b'BNTX\x00\x00\x00\x00', pos)
        if b < 0:
            return
        pos = b + 8
        if blob[b + 0x20:b + 0x24] != b'NX  ':
            continue
        count = struct.unpack_from('<I', blob, b + 0x24)[0]
        info_arr = struct.unpack_from('<Q', blob, b + 0x28)[0]
        if count > 100000 or b + info_arr + 8 * count > len(blob):
            continue
        for i in range(count):
            t = b + struct.unpack_from('<Q', blob, b + info_arr + 8 * i)[0]
            if blob[t:t + 4] != b'BRTI':
                continue
            (flags, dim, tile, swz, mips, samples, fmt, access, w, h, d, arr, layout, layout2) = \
                struct.unpack_from('<BBHHHIIIIIIIII', blob, t + 0x10)
            image_size, align, comp, ttype = struct.unpack_from('<IIII', blob, t + 0x50)
            name_off, _, ptrs = struct.unpack_from('<QQQ', blob, t + 0x60)
            name = ''
            if 0 < name_off < len(blob) - b:
                n = struct.unpack_from('<H', blob, b + name_off)[0]
                name = blob[b + name_off + 2:b + name_off + 2 + n].decode('utf8', 'replace')
            offs = list(struct.unpack_from('<%dQ' % mips, blob, b + ptrs)) if 0 < ptrs < len(blob) - b and mips <= 16 else []
            yield name, dict(dim=dim, tile=tile, mips=mips, samples=samples, fmt=fmt, w=w, h=h, d=d, arr=arr,
                             bh=layout & 7, size=image_size, align=align, ttype=ttype, offs=offs)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--limit', type=int, default=0, help='stop after this many archives (0 = all)')
    ap.add_argument('--show', type=int, default=12)
    a = ap.parse_args()
    lib = build_probe()
    cap = 256 << 20
    buf = ctypes.create_string_buffer(cap)
    stats = collections.Counter()
    bad = collections.defaultdict(list)
    unmapped = collections.Counter()
    archives = []
    for root, _, files in os.walk(ROMFS):
        for f in files:
            if f.endswith('.szs'):
                archives.append(os.path.join(root, f))
    archives.sort()
    if a.limit:
        archives = archives[:a.limit]
    for k, path in enumerate(archives):
        src = open(path, 'rb').read()
        n = lib.yaz0_decompress(src, len(src), buf, cap)
        if n < 0:
            stats['archives not decodable'] += 1
            continue
        stats['archives'] += 1
        blob = buf.raw[:n]
        for name, t in textures(blob):
            stats['textures'] += 1
            typ = t['fmt'] >> 8
            if typ not in NVN_FOR_TYPE:
                unmapped['0x%04x' % t['fmt']] += 1
                continue
            if t['tile'] != 0:
                stats['not block-linear (tile mode %d), skipped' % t['tile']] += 1
                continue
            target = {1: 0, 2: 1, 3: 2}.get(t['dim'], 1)
            depth = t['d']
            if t['arr'] > 1:
                target, depth = (4, t['arr']) if target == 1 else (3, t['arr'])
            o = ProbeOut()
            lib.probe_layout(target, NVN_FOR_TYPE[typ], t['w'], t['h'], depth, t['mips'], 0, ctypes.byref(o))
            stats['compared'] += 1
            where = '%s:%s %dx%dx%d arr %d mips %d fmt 0x%04x' % (os.path.relpath(path, ROMFS), name, t['w'], t['h'], t['d'], t['arr'], t['mips'], t['fmt'])
            # The BRTI alignment field is NOT compared: it is 512 for every texture regardless of block height (5,442 of
            # 5,442 sampled, block heights 1..16 GOBs), i.e. the container's data alignment, not NVN's storage alignment.
            # Ours (512 << block height log2) is always a multiple of it, which is what the container requires.
            stats['alignment: ours a multiple of the BNTX field' if o.alignment % max(t['align'], 1) == 0
                  else 'alignment: ours NOT a multiple of the BNTX field'] += 1
            checks = [('size', o.data_size, t['size']), ('block height', o.bh_log2, t['bh'])]
            if len(t['offs']) == t['mips'] and t['offs']:
                rel = [x - t['offs'][0] for x in t['offs']]
                checks.append(('mip offsets', [o.lvl_offset[i] for i in range(min(t['mips'], o.levels))], rel[:o.levels]))
            ok_all = True
            for what, got, want in checks:
                if got == want:
                    stats[what + ' equal'] += 1
                else:
                    ok_all = False
                    stats[what + ' DIFFERENT'] += 1
                    if len(bad[what]) < a.show:
                        bad[what].append('%s: ours %s, BNTX %s' % (where, got, want))
            stats['all fields equal' if ok_all else 'some field different'] += 1
        if (k + 1) % 500 == 0:
            print('  %d/%d archives, %d textures' % (k + 1, len(archives), stats['textures']), flush=True)
    for key in sorted(stats):
        print('%-48s %d' % (key, stats[key]))
    if unmapped:
        print('unmapped formats (not compared):', ', '.join('%s x%d' % kv for kv in unmapped.most_common()))
    for what, lines in bad.items():
        print('examples, %s:' % what)
        for l in lines:
            print('  ' + l)
    sys.exit(0 if stats['some field different'] == 0 and stats['compared'] > 0 else 1)


if __name__ == '__main__':
    main()
