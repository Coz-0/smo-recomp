"""Copy a save folder so that its game file continues in another stage: a test can start in a late kingdom without
playing there. The source folder is only read; the copy is written to a new folder (never point --out at your own save).

    python tools/dev/save_warp.py <save folder> --out <new folder> --stage BossRaidWorldHomeStage [--world 11] [--file File1.bin]

The game file (FileN.bin) is a 16-byte header (CRC-32 of everything after its first 4 bytes, version, ..., size of the
data) and a little-endian BYML (version 3) dictionary. The copy gets StageName = the stage, WorldId = its world (from
SystemData/WorldList.szs in build/romfs when --world is not given) and an empty CheckpointName, so the game starts the
stage at its default entrance. A stage name that is not in the file's string table is inserted in sorted order (the
indices of the strings after it move up by one, every offset behind the table moves by the table's growth).
"""
import argparse
import os
import shutil
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, 'tools'))
import target as T  # noqa: E402  (SMO_TARGET: build/ or build/<update version>/)
OFFSET_TYPES = (0xC0, 0xC1, 0xD4, 0xD5, 0xD6)          # node types whose value is an offset to the node's data


def strtab(d, off):
    n = struct.unpack_from('<I', d, off)[0] >> 8
    offs = struct.unpack_from('<%dI' % (n + 1), d, off + 4)
    return [d[off + offs[i]:d.index(b'\0', off + offs[i])] for i in range(n)], offs[n]


def build_strtab(strings):
    n = len(strings)
    head = 4 + 4 * (n + 1)
    offs, body = [], b''
    for s in strings:
        offs.append(head + len(body))
        body += s + b'\0'
    offs.append(head + len(body))
    t = struct.pack('<I', 0xC2 | (n << 8)) + struct.pack('<%dI' % (n + 1), *offs) + body
    return t + b'\0' * (-len(t) % 4)


def slots(d, root_off):
    """(position, type) of every value slot of every array and dictionary reachable from the root."""
    out, seen, todo = [], set(), [(d[root_off], root_off)]
    while todo:
        t, v = todo.pop()
        if t not in (0xC0, 0xC1) or v in seen:
            continue
        seen.add(v)
        n = struct.unpack_from('<I', d, v)[0] >> 8
        if t == 0xC0:
            types = d[v + 4:v + 4 + n]
            base = v + 4 + ((n + 3) & ~3)
            items = [(types[i], base + 4 * i) for i in range(n)]
        else:
            items = [(d[v + 4 + 8 * i + 3], v + 4 + 8 * i + 4) for i in range(n)]
        for typ, pos in items:
            out.append((pos, typ))
            todo.append((typ, struct.unpack_from('<I', d, pos)[0]))
    return out


def root_entry(d, root_off, keys, name):
    """Position of the value slot and the type of key `name` in the root dictionary."""
    n = struct.unpack_from('<I', d, root_off)[0] >> 8
    for i in range(n):
        w, = struct.unpack_from('<I', d, root_off + 4 + 8 * i)
        if keys[w & 0xffffff] == name:
            return root_off + 4 + 8 * i + 4, w >> 24
    raise SystemExit('the save has no %s' % name.decode())


def world_of(stage):
    import sarc as S
    from byml import Byml, sarc_files
    p = T.build('romfs', 'SystemData', 'WorldList.szs')
    files = sarc_files(S.yaz0(open(p, 'rb').read()))
    for i, w in enumerate(Byml(files['WorldList.byml']).root()):
        if w.get('Name') == stage:
            return i
    raise SystemExit('%s is not the home stage of a world: give --world' % stage)


def warp(d, stage, world):
    if d[:2] != b'YB' or struct.unpack_from('<H', d, 2)[0] not in (2, 3):
        raise SystemExit('not a little-endian BYML version 2 or 3')
    key_off, str_off, root_off = struct.unpack_from('<III', d, 4)
    if not key_off < str_off < root_off:
        raise SystemExit('unexpected BYML layout (key table, string table, data)')
    keys, _ = strtab(d, key_off)
    strings, end_rel = strtab(d, str_off)
    old_end = (str_off + end_rel + 3) & ~3
    new = stage.encode()
    remap = list(range(len(strings)))
    if new not in strings:
        at = next((i for i, s in enumerate(strings) if s > new), len(strings))
        strings.insert(at, new)
        remap = [i if i < at else i + 1 for i in remap]
    table = build_strtab(strings)
    delta = str_off + len(table) - old_end
    sl = slots(d, root_off)
    out = bytearray(d[:str_off] + table + d[old_end:])
    struct.pack_into('<I', out, 12, root_off + delta)

    def fix(v):
        return v + delta if v >= old_end else v
    for pos, typ in sl:
        p = pos + delta
        v, = struct.unpack_from('<I', out, p)
        if typ == 0xA0:
            struct.pack_into('<I', out, p, remap[v])
        elif typ in OFFSET_TYPES:
            struct.pack_into('<I', out, p, fix(v))
    root = root_off + delta
    for name, typ_want, value in ((b'StageName', 0xA0, strings.index(new)), (b'CheckpointName', 0xA0, strings.index(b'')),
                                  (b'WorldId', 0xD1, world)):
        pos, typ = root_entry(out, root, keys, name)
        if typ != typ_want:
            raise SystemExit('%s has node type %02x, expected %02x' % (name.decode(), typ, typ_want))
        struct.pack_into('<I' if typ == 0xA0 else '<i', out, pos, value)
    return bytes(out), len(sl)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('save', help='folder holding the save (Common.bin, FileN.bin)')
    ap.add_argument('--out', required=True, help='new folder for the copy (must not exist)')
    ap.add_argument('--stage', required=True)
    ap.add_argument('--world', type=int)
    ap.add_argument('--file', help='game file to change (default: the only FileN.bin)')
    a = ap.parse_args()
    if os.path.exists(a.out):
        sys.exit('%s exists: give a new folder' % a.out)
    if os.path.abspath(a.out) == os.path.abspath(a.save):
        sys.exit('--out must be a new folder')
    name = a.file or next(n for n in sorted(os.listdir(a.save)) if n.startswith('File') and n.endswith('.bin'))
    world = a.world if a.world is not None else world_of(a.stage)
    raw = open(os.path.join(a.save, name), 'rb').read()
    crc, ver, unk, size = struct.unpack_from('<IIII', raw, 0)
    if zlib.crc32(raw[4:]) & 0xffffffff != crc:
        sys.exit('%s: the CRC-32 in the header does not match: not a game file of the kind this tool knows' % name)
    data, nslots = warp(raw[16:16 + size], a.stage, world)
    body = struct.pack('<III', ver, unk, len(data)) + data
    if len(body) + 4 > len(raw):
        sys.exit('the new data does not fit the file')
    body += b'\0' * (len(raw) - 4 - len(body))
    shutil.copytree(a.save, a.out)
    with open(os.path.join(a.out, name), 'wb') as f:
        f.write(struct.pack('<I', zlib.crc32(body) & 0xffffffff) + body)
    print('%s: %s -> stage %s, world %d (%d value slots checked, data %d -> %d bytes)' % (a.out, name, a.stage, world, nslots, size, len(data)))


if __name__ == '__main__':
    main()
