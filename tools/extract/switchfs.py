"""Read-only access to the Switch package formats of the user's own dump: NSP (PFS0), NCA, ExeFS (PFS0), RomFS, CNMT,
NACP, NSO.  Adapted from ssbu-recomp tools/extract (romfs_extract.py, nca_exefs.py, nso2elf.py), GPL-3.0-or-later.

Nothing here writes anything; callers decide where derived files go (always under build/, ignored by git).

Keys: the user's own prod.keys, looked up in this order (the file is only read, never copied or printed):
    --keys <path> (passed as `keys_path`), %SMO_KEYS%, ~/.switch/prod.keys, %APPDATA%/Ryujinx/system/prod.keys
Title keys come from the ticket inside the NSP; title.keys next to prod.keys is the fallback.
"""
import hashlib
import os
import struct

from Crypto.Cipher import AES

CONTENT_TYPES = {0: 'Program', 1: 'Meta', 2: 'Control', 3: 'Manual', 4: 'Data', 5: 'PublicData'}


# ------------------------------------------------------------------------------------------------------------- keys
def find_keys(keys_path=None):
    cands = [keys_path, os.environ.get('SMO_KEYS'), os.path.expanduser('~/.switch/prod.keys')]
    if os.environ.get('APPDATA'):
        cands.append(os.path.join(os.environ['APPDATA'], 'Ryujinx', 'system', 'prod.keys'))
    for p in cands:
        if p and os.path.isfile(p):
            return p
    raise FileNotFoundError('prod.keys not found (tried: %s)' % ', '.join(p for p in cands if p))


def _read_keyfile(path):
    out = {}
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            if '=' in line:
                k, v = line.split('=', 1)
                try:
                    out[k.strip().lower()] = bytes.fromhex(v.strip())
                except ValueError:
                    pass
    return out


class Keys:
    def __init__(self, keys_path=None):
        self.path = find_keys(keys_path)
        self.k = _read_keyfile(self.path)
        tk = os.path.join(os.path.dirname(self.path), 'title.keys')
        self.title = _read_keyfile(tk) if os.path.isfile(tk) else {}

    def __getitem__(self, name):
        if name not in self.k:
            raise KeyError('key "%s" missing from %s' % (name, self.path))
        return self.k[name]


# ------------------------------------------------------------------------------------------------------------- crypto
def xts_decrypt(key, data, sector, sector_size=0x200):
    """AES-128-XTS with the Nintendo (big-endian) sector tweak."""
    k1, k2 = AES.new(key[:16], AES.MODE_ECB), AES.new(key[16:], AES.MODE_ECB)
    out = bytearray()
    for s in range(len(data) // sector_size):
        tweak = int.from_bytes(k2.encrypt((sector + s).to_bytes(16, 'big')), 'little')
        for b in range(0, sector_size, 16):
            block = data[s * sector_size + b:s * sector_size + b + 16]
            t = tweak.to_bytes(16, 'little')
            out += bytes(x ^ y for x, y in zip(k1.decrypt(bytes(x ^ y for x, y in zip(block, t))), t))
            tweak <<= 1
            if tweak >> 128:
                tweak = (tweak & ((1 << 128) - 1)) ^ 0x87
    return bytes(out)


# ------------------------------------------------------------------------------------------------------------- PFS0
class Pfs0:
    """A PFS0 over any reader `read(offset, size)`."""

    def __init__(self, read):
        self._read = read
        magic, count, strings_size, _ = struct.unpack('<4sIII', read(0, 16))
        assert magic == b'PFS0', magic
        table = read(16, 24 * count + strings_size)
        strings = table[24 * count:]
        base = 16 + 24 * count + strings_size
        self.files = {}
        for i in range(count):
            off, size, name_off, _ = struct.unpack_from('<QQII', table, i * 24)
            name = strings[name_off:strings.index(b'\0', name_off)].decode()
            self.files[name] = (base + off, size)

    def read(self, name, offset=0, size=None):
        base, total = self.files[name]
        if size is None:
            size = total - offset
        return self._read(base + offset, size)


class Nsp(Pfs0):
    def __init__(self, path):
        self.path = path
        self.f = open(path, 'rb')

        def rd(off, size):
            self.f.seek(off)
            return self.f.read(size)
        Pfs0.__init__(self, rd)

    def tickets(self):
        return [self.read(n) for n in self.files if n.endswith('.tik')]


# ------------------------------------------------------------------------------------------------------------- NCA
class Nca:
    def __init__(self, nsp, name, keys):
        self.nsp, self.name, self.keys = nsp, name, keys
        h = xts_decrypt(keys['header_key'], nsp.read(name, 0, 0xC00), 0)
        if h[0x200:0x204] != b'NCA3':
            raise ValueError('%s: not NCA3 after header decryption (%r): wrong header_key?' % (name, h[0x200:0x204]))
        self.header = h
        self.content_type = h[0x205]
        self.key_gen = max(max(h[0x206], h[0x220]) - 1, 0)
        self.size = struct.unpack_from('<Q', h, 0x208)[0]
        self.title_id = struct.unpack_from('<Q', h, 0x210)[0]
        self.sdk_version = struct.unpack_from('<I', h, 0x21C)[0]
        self.rights_id = h[0x230:0x240]
        self.sections = []
        for i in range(4):
            start, end = struct.unpack_from('<II', h, 0x240 + i * 16)
            if end:
                fs = h[0x400 + i * 0x200:0x600 + i * 0x200]
                self.sections.append({'index': i, 'offset': start * 0x200, 'size': (end - start) * 0x200, 'fs': fs,
                                      'fs_type': fs[2], 'hash_type': fs[3], 'crypto': fs[4],
                                      'ctr': fs[0x140:0x148][::-1]})
        self._key = None

    @property
    def key(self):
        if self._key is None:
            if any(self.rights_id):
                tkey = None
                for tik in self.nsp.tickets():
                    if tik[0x2A0:0x2B0] == self.rights_id:
                        tkey = tik[0x180:0x190]
                if tkey is None:
                    tkey = self.keys.title.get(self.rights_id.hex())
                if tkey is None:
                    raise KeyError('no ticket / title key for rights id %s' % self.rights_id.hex())
                self._key = AES.new(self.keys['titlekek_%02x' % self.key_gen], AES.MODE_ECB).decrypt(tkey)
            else:
                kaek = ('application', 'ocean', 'system')[self.header[0x207]] if self.header[0x207] < 3 else 'application'
                area = AES.new(self.keys['key_area_key_%s_%02x' % (kaek, self.key_gen)], AES.MODE_ECB).decrypt(
                    self.header[0x300:0x340])
                self._key = area[0x20:0x30]
        return self._key

    def read(self, sec, offset, size, ctr_val=None):
        """Decrypted read of `size` bytes at a section-relative offset (AES-CTR sections)."""
        if size <= 0:
            return b''
        absolute = sec['offset'] + offset
        aligned = absolute & ~0xF
        data = self.nsp.read(self.name, aligned, (absolute - aligned + size + 15) & ~0xF)
        if sec['crypto'] == 1:                                     # plain
            return data[absolute - aligned:absolute - aligned + size]
        upper = sec['ctr'] if ctr_val is None else sec['ctr'][:4] + struct.pack('>I', ctr_val)
        counter = int.from_bytes(upper + struct.pack('>Q', aligned >> 4), 'big')
        plain = AES.new(self.key, AES.MODE_CTR, nonce=b'', initial_value=counter).decrypt(data)
        return plain[absolute - aligned:absolute - aligned + size]

    def pfs0(self, sec=None):
        """The PFS0 of a PFS0 section (ExeFS, CNMT): data is region 1 of the HierarchicalSha256 hash data."""
        sec = sec or next(s for s in self.sections if s['fs_type'] == 1)
        pfs_off, _ = struct.unpack_from('<QQ', sec['fs'], 0x40)
        return Pfs0(lambda o, n: self.read(sec, pfs_off + o, n))

    def romfs(self, sec=None):
        """(reader, base) of a plain (non-BKTR) RomFS section: data is IVFC level 6."""
        sec = sec or next(s for s in self.sections if s['fs_type'] == 0)
        fs = sec['fs']
        assert fs[0x8:0xC] == b'IVFC', fs[0x8:0xC]
        level6 = struct.unpack_from('<Q', fs, 0x8 + 0x10 + 5 * 0x18)[0]
        return (lambda o, n: self.read(sec, o, n)), level6


def romfs_walk(read, base):
    """Yield (path, absolute data offset, size) for every file of a RomFS image at `base`."""
    hdr = struct.unpack('<10Q', read(base, 0x50))
    assert hdr[0] == 0x50, 'RomFS header not found'
    dir_off, dir_size, file_off, file_size, data_off = hdr[3], hdr[4], hdr[7], hdr[8], hdr[9]
    dirs = read(base + dir_off, dir_size)
    files = read(base + file_off, file_size)
    stack = [(0, '')]
    while stack:
        d, path = stack.pop()
        _, sibling, child, first_file, _, name_len = struct.unpack_from('<6I', dirs, d)
        name = dirs[d + 24:d + 24 + name_len].decode('utf8')
        here = path + '/' + name if name else path
        if sibling != 0xFFFFFFFF:
            stack.append((sibling, path))
        if child != 0xFFFFFFFF:
            stack.append((child, here))
        f = first_file
        while f != 0xFFFFFFFF:
            _, next_file, off, size, _, name_len = struct.unpack_from('<IIQQII', files, f)
            yield here + '/' + files[f + 32:f + 32 + name_len].decode('utf8'), base + data_off + off, size
            f = next_file


# ------------------------------------------------------------------------------------------------------------- package
class Package:
    """One NSP: its NCAs by content type."""

    def __init__(self, path, keys):
        self.nsp = Nsp(path)
        self.ncas = []
        for n in sorted(self.nsp.files):
            if n.endswith('.nca'):
                self.ncas.append(Nca(self.nsp, n, keys))

    def by_type(self, ctype):
        return [n for n in self.ncas if CONTENT_TYPES.get(n.content_type) == ctype]

    def meta(self):
        """Packaged content meta: title id, version, type (from the .cnmt inside the Meta NCA)."""
        m = self.by_type('Meta')[0]
        pfs = m.pfs0()
        name = next(n for n in pfs.files if n.endswith('.cnmt'))
        d = pfs.read(name)
        tid, ver, mtype = struct.unpack_from('<QIB', d, 0)
        return {'title_id': '%016x' % tid, 'version': ver, 'meta_type': mtype}

    def nacp(self):
        c = self.by_type('Control')[0]
        read, base = c.romfs()
        for path, off, size in romfs_walk(read, base):
            if path == '/control.nacp':
                return read(off, size)
        raise FileNotFoundError('control.nacp')


def nacp_info(d):
    names = []
    for i in range(16):
        n = d[i * 0x300:i * 0x300 + 0x200].split(b'\0')[0].decode('utf8', 'replace')
        if n:
            names.append(n)
    return {'name': names[0] if names else '',
            'display_version': d[0x3060:0x3070].split(b'\0')[0].decode('ascii', 'replace'),
            'user_save_size': struct.unpack_from('<q', d, 0x3080)[0],
            'user_save_journal': struct.unpack_from('<q', d, 0x3088)[0]}


def sdk_version_str(v):
    return '%d.%d.%d.%d' % (v >> 24 & 0xff, v >> 16 & 0xff, v >> 8 & 0xff, v & 0xff)


# ------------------------------------------------------------------------------------------------------------- NSO
def nso_info(d):
    """Segments (decompressed), build id and integrity of an NSO."""
    import lz4.block
    assert d[:4] == b'NSO0'
    flags = struct.unpack_from('<I', d, 12)[0]
    segs = []
    for i, (ho, cso) in enumerate(((0x10, 0x60), (0x20, 0x64), (0x30, 0x68))):
        foff, moff, size = struct.unpack_from('<III', d, ho)
        csz = struct.unpack_from('<I', d, cso)[0]
        raw = d[foff:foff + csz]
        if flags & (1 << i):
            raw = lz4.block.decompress(raw, uncompressed_size=size)
        assert len(raw) == size
        ok = hashlib.sha256(raw).digest() == d[0xA0 + i * 0x20:0xC0 + i * 0x20]
        segs.append({'vaddr': moff, 'size': size, 'data': raw, 'sha_ok': ok})
    return {'segments': segs, 'bss': struct.unpack_from('<I', d, 0x3c)[0], 'build_id': d[0x40:0x60].hex()}


def nso_to_elf(d):
    """NSO -> ELF64 (sections + program headers, image at file offset 0x1000). Port of ssbu-recomp nso2elf.py."""
    info = nso_info(d)
    (tv, text), (rv, ro), (dv, data) = [(s['vaddr'], s['data']) for s in info['segments']]
    bss = info['bss']
    mem = bytearray(dv + len(data))
    for v, r in ((tv, text), (rv, ro), (dv, data)):
        mem[v:v + len(r)] = r
    mod = struct.unpack_from('<I', text, 4)[0]
    assert mem[mod:mod + 4] == b'MOD0'
    dyn_off, bss_s, bss_e = struct.unpack_from('<iii', mem, mod + 4)
    dyn = mod + dyn_off
    tags, n = {}, 0
    while True:
        t, v = struct.unpack_from('<qQ', mem, dyn + n * 16)
        n += 1
        if t == 0:
            break
        tags.setdefault(t, v)
    dynsz = n * 16
    symtab, strtab, strsz = tags[6], tags[5], tags[10]
    nsym = struct.unpack_from('<I', mem, tags[4] + 4)[0] if 4 in tags else (strtab - symtab) // 24
    OFF = 0x1000
    names = [b'\0']
    secs = [bytes(64)]

    def sec(name, typ, fl, addr, off, size, link=0, info_=0, align=1, ent=0):
        ni = sum(len(x) for x in names)
        names.append(name.encode() + b'\0')
        secs.append(struct.pack('<IIQQQQIIQQ', ni, typ, fl, addr, off, size, link, info_, align, ent))
    sec('.text', 1, 6, tv, OFF + tv, len(text), align=16)
    assert rv <= symtab and symtab + nsym * 24 <= strtab
    sec('.rodata', 1, 2, rv, OFF + rv, symtab - rv, align=16)
    sec('.dynsym', 11, 2, symtab, OFF + symtab, nsym * 24, link=4, info_=1, align=8, ent=24)
    sec('.dynstr', 3, 2, strtab, OFF + strtab, strsz)
    r1 = strtab + strsz
    sec('.rodata.1', 1, 2, r1, OFF + r1, rv + len(ro) - r1)
    sec('.data', 1, 3, dv, OFF + dv, dyn - dv, align=16)
    sec('.dynamic', 6, 3, dyn, OFF + dyn, dynsz, link=4, align=8, ent=16)
    d1 = dyn + dynsz
    sec('.data.1', 1, 3, d1, OFF + d1, dv + len(data) - d1)
    sec('.bss', 8, 3, bss_s, OFF + dv + len(data), bss_e - bss_s, align=16)
    shstr_i = len(secs)
    shstr_off = OFF + len(mem)
    sec('.shstrtab', 3, 0, 0, shstr_off, 0)
    blob = b''.join(names)
    secs[-1] = secs[-1][:32] + struct.pack('<Q', len(blob)) + secs[-1][40:]
    shoff = (shstr_off + len(blob) + 7) & ~7

    def phdr(t, fl, v, fsz, msz, al):
        return struct.pack('<IIQQQQQQ', t, fl, OFF + v, v, v, fsz, msz, al)
    ph = (phdr(1, 5, tv, len(text), len(text), 0x1000) + phdr(1, 4, rv, len(ro), len(ro), 0x1000) +
          phdr(1, 6, dv, len(data), max(len(data), bss_e - dv, len(data) + bss), 0x1000) +
          phdr(2, 6, dyn, dynsz, dynsz, 8))
    eh = b'\x7fELF\x02\x01\x01' + bytes(9) + struct.pack('<HHIQQQIHHHHHH', 3, 183, 1, 0, 0x40, shoff, 0, 0x40, 0x38, 4,
                                                          0x40, len(secs), shstr_i)
    out = bytearray(eh + ph)
    out += bytes(OFF - len(out))
    out += mem
    out += blob
    out += bytes(shoff - len(out))
    out += b''.join(secs)
    meta = {'text': [tv, len(text)], 'rodata': [rv, len(ro)], 'data': [dv, len(data)], 'bss': [bss_s, bss_e - bss_s],
            'dynamic': dyn, 'dynsym': symtab, 'nsym': nsym, 'dynstr': strtab, 'mod0': mod, 'build_id': info['build_id'],
            'segments_sha_ok': all(s['sha_ok'] for s in info['segments']),
            'dt': {str(k): v for k, v in tags.items()}}
    return bytes(out), meta
