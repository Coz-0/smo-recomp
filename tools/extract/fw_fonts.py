"""Extract the console's shared system fonts (nn::pl) from the user's own installed firmware.

    python tools/extract/fw_fonts.py [--fw <folder of .nca>] [--out build/sysdata/fonts] [--keys <prod.keys>]

Adapted from ssbu-recomp tools/extract/fw_fonts.py (GPL-3.0-or-later). The fonts live in the system-data titles
0100000000000810..0100000000000814 of the firmware. Default input: the firmware Ryujinx installed for the user
(%APPDATA%/Ryujinx/bis/system/Contents/registered, where each NCA is a folder "<id>.nca" holding a file "00");
a plain folder of .nca files works too. Each .bfttf is decoded (every big-endian word XORed with a key that follows
from the known header word) and written as .ttf only if the result starts with a TrueType / OpenType signature.
Output is derived from the user's firmware and stays under build/ (ignored by git).
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import switchfs as S

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FONT_TITLES = set(range(0x0100000000000810, 0x0100000000000815))
SFNT = (b'\x00\x01\x00\x00', b'OTTO', b'ttcf', b'true')
BFTTF_PLAIN_MAGIC = 0x7f9a0218


class LooseFile:
    """The read(name, offset, size) interface switchfs.Nca expects from an NSP, over one loose NCA file."""

    def __init__(self, path):
        self.f = open(path, 'rb')

    def read(self, name, offset, size):
        self.f.seek(offset)
        return self.f.read(size)

    def tickets(self):
        return []


def decode_bfttf(data):
    words = struct.unpack('>%dI' % (len(data) // 4), data[:len(data) // 4 * 4])
    key = words[0] ^ BFTTF_PLAIN_MAGIC
    size = words[1] ^ key
    out = struct.pack('>%dI' % (len(words) - 2), *[w ^ key for w in words[2:]])
    return out[:size] if size <= len(out) else b''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--fw', default=os.path.join(os.environ.get('APPDATA', ''), 'Ryujinx', 'bis', 'system', 'Contents', 'registered'))
    ap.add_argument('--out', default=os.path.join(REPO, 'build', 'sysdata', 'fonts'))
    ap.add_argument('--keys')
    a = ap.parse_args()
    keys = S.Keys(a.keys)
    os.makedirs(a.out, exist_ok=True)
    found = 0
    for name in sorted(os.listdir(a.fw)):
        if not name.endswith('.nca') or name.endswith('.cnmt.nca'):
            continue
        p = os.path.join(a.fw, name)
        if os.path.isdir(p):
            p = os.path.join(p, '00')
        if not os.path.isfile(p):
            continue
        try:
            nca = S.Nca(LooseFile(p), name, keys)
        except (ValueError, KeyError):
            continue
        if nca.title_id not in FONT_TITLES:
            continue
        sec = next((s for s in nca.sections if s['fs_type'] == 0), None)
        if sec is None:
            continue
        read, base = nca.romfs(sec)
        for path, offset, size in S.romfs_walk(read, base):
            raw = read(offset, size)
            fname = os.path.basename(path)
            decoded = decode_bfttf(raw) if fname.endswith('.bfttf') else raw
            ok = decoded[:4] in SFNT
            target = os.path.join(a.out, os.path.splitext(fname)[0] + '.ttf')
            if ok:
                open(target, 'wb').write(decoded)
                found += 1
            print('%016x  %-42s %9d bytes  %s' % (nca.title_id, path, size,
                  'decoded OK (%s)' % decoded[:4].hex() if ok else 'NOT a valid font after decoding: %s' % decoded[:8].hex()))
    print('%d fonts written to %s' % (found, a.out))
    sys.exit(0 if found else 1)


if __name__ == '__main__':
    main()
