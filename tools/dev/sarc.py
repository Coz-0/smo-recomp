"""List the files inside SARC archives of the extracted RomFS (Yaz0-compressed .szs or plain), names and sizes only.

    python tools/dev/sarc.py <romfs-relative path> [...]        e.g. ObjectData/CapManHero.szs

Reads build/romfs (generated locally by tools/extract/romfs.py); prints nothing of the files' contents.
"""
import os
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, 'tools'))
import target as T  # noqa: E402  (SMO_TARGET: build/ or build/<update version>/)


def yaz0(d):
    if d[:4] != b'Yaz0':
        return d
    size = struct.unpack_from('>I', d, 4)[0]
    out = bytearray()
    s = 16
    while len(out) < size:
        code = d[s]; s += 1
        for bit in range(8):
            if len(out) >= size:
                break
            if code & (0x80 >> bit):
                out.append(d[s]); s += 1
            else:
                b1, b2 = d[s], d[s + 1]; s += 2
                dist = ((b1 & 0xf) << 8 | b2) + 1
                n = b1 >> 4
                if n == 0:
                    n = d[s] + 0x12; s += 1
                else:
                    n += 2
                for _ in range(n):
                    out.append(out[-dist])
    return bytes(out)


def sarc(d):
    assert d[:4] == b'SARC', 'not a SARC'
    bom = d[6:8]
    e = '<' if bom == b'\xff\xfe' else '>'
    hdr, = struct.unpack_from(e + 'H', d, 4)
    data_off, = struct.unpack_from(e + 'I', d, 0xc)
    sfat = hdr
    nodes, = struct.unpack_from(e + 'H', d, sfat + 6)
    sfnt = sfat + 0xc + nodes * 0x10
    names = sfnt + 8
    out = []
    for i in range(nodes):
        h, attr, beg, end = struct.unpack_from(e + 'IIII', d, sfat + 0xc + i * 0x10)
        name = ''
        if attr & 0x01000000:
            o = names + (attr & 0xffff) * 4
            name = d[o:d.index(b'\0', o)].decode('utf-8', 'replace')
        out.append((name, end - beg))
    return out


def main():
    for p in sys.argv[1:]:
        full = T.build('romfs', p)
        for name, size in sarc(yaz0(open(full, 'rb').read())):
            print('%s\t%s\t%d' % (p, name, size))


if __name__ == '__main__':
    main()
