"""List the placed objects of a stage map (BYML inside a SARC of the extracted RomFS): name, id and position, per list.

    python tools/dev/byml.py StageData/CapWorldHomeStageMap.szs [--file CapWorldHomeStageMap.byml] [--grep Frog] [--scenario N]

Reads build/romfs (generated locally by tools/extract/romfs.py) and prints only names and coordinates to the console,
as a guide for test input scripts; nothing of the game's data is written anywhere. BYML version 2/3, little-endian.
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sarc as S  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, 'tools'))
import target as T  # noqa: E402  (SMO_TARGET: build/ or build/<update version>/)


class Byml:
    def __init__(self, d):
        if d[:2] != b'YB':
            raise SystemExit('not a little-endian BYML')
        self.d = d
        self.keys = self.strtab(struct.unpack_from('<I', d, 4)[0])
        self.strs = self.strtab(struct.unpack_from('<I', d, 8)[0])
        self.root_off = struct.unpack_from('<I', d, 12)[0]

    def strtab(self, off):
        if not off:
            return []
        d = self.d
        n = struct.unpack_from('<I', d, off)[0] >> 8
        offs = struct.unpack_from('<%dI' % (n + 1), d, off + 4)
        return [d[off + offs[i]:d.index(b'\0', off + offs[i])].decode('utf-8', 'replace') for i in range(n)]

    def node(self, t, v):
        d = self.d
        if t == 0xA0:
            return self.strs[v]
        if t == 0xC0:
            n = struct.unpack_from('<I', d, v)[0] >> 8
            types = d[v + 4:v + 4 + n]
            base = v + 4 + ((n + 3) & ~3)
            return [self.node(types[i], struct.unpack_from('<I', d, base + 4 * i)[0]) for i in range(n)]
        if t == 0xC1:
            n = struct.unpack_from('<I', d, v)[0] >> 8
            out = {}
            for i in range(n):
                w, val = struct.unpack_from('<II', d, v + 4 + 8 * i)
                out[self.keys[w & 0xffffff]] = self.node(w >> 24, val)
            return out
        if t == 0xD0:
            return bool(v)
        if t == 0xD1:
            return struct.unpack('<i', struct.pack('<I', v))[0]
        if t == 0xD2:
            return struct.unpack('<f', struct.pack('<I', v))[0]
        if t == 0xD3:
            return v
        if t in (0xD4, 0xD5):
            return struct.unpack_from('<q' if t == 0xD4 else '<Q', d, v)[0]
        if t == 0xD6:
            return struct.unpack_from('<d', d, v)[0]
        if t == 0xFF:
            return None
        return '<type %02x>' % t

    def root(self):
        t = self.d[self.root_off]
        return self.node(t, self.root_off)


def sarc_files(d):
    """{name: bytes} of a SARC archive (sarc.py lists names and sizes only)."""
    assert d[:4] == b'SARC', 'not a SARC'
    e = '<' if d[6:8] == b'\xff\xfe' else '>'
    hdr, = struct.unpack_from(e + 'H', d, 4)
    data_off, = struct.unpack_from(e + 'I', d, 0xc)
    nodes, = struct.unpack_from(e + 'H', d, hdr + 6)
    names = hdr + 0xc + nodes * 0x10 + 8
    out = {}
    for i in range(nodes):
        h, attr, beg, end = struct.unpack_from(e + 'IIII', d, hdr + 0xc + i * 0x10)
        if attr & 0x01000000:
            o = names + (attr & 0xffff) * 4
            out[d[o:d.index(b'\0', o)].decode('utf-8', 'replace')] = d[data_off + beg:data_off + end]
    return out


def pos(o):
    t = o.get('Translate') or {}
    return '%9.1f %9.1f %9.1f' % (t.get('X', 0), t.get('Y', 0), t.get('Z', 0))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('archive')
    ap.add_argument('--file')
    ap.add_argument('--grep', default='')
    ap.add_argument('--scenario', type=int, default=-1, help='0-based index in the root array (default: all)')
    a = ap.parse_args()
    data = S.yaz0(open(T.build('romfs', a.archive), 'rb').read())
    files = sarc_files(data)
    name = a.file or next(n for n in files if n.endswith('Map.byml'))
    root = Byml(files[name]).root()
    scen = root if isinstance(root, list) else [root]
    for si, s in enumerate(scen):
        if a.scenario >= 0 and si != a.scenario:
            continue
        for lst, objs in sorted(s.items()) if isinstance(s, dict) else []:
            if not isinstance(objs, list):
                continue
            for o in objs:
                if not isinstance(o, dict):
                    continue
                nm = '%s %s' % (o.get('UnitConfigName', '?'), o.get('Id', ''))
                if a.grep.lower() in (nm + ' ' + lst).lower():
                    print('scenario %d %-14s %-44s %s' % (si, lst, nm, pos(o)))


if __name__ == '__main__':
    main()
