"""The few Switch file formats setup reads from your decrypted dump, none of them encrypted: NSO executables
(LZ4-compressed segments) converted to ELF, the program's NPDM and NACP, and the console's .bfttf font wrapper.

nso_info / nso_to_elf are adapted from ssbu-recomp tools/extract (nso2elf.py), GPL-3.0-or-later.
Nothing here writes anything; callers decide where derived files go (always under build/, ignored by git).
"""
import hashlib
import re
import struct

OFF = 0x1000                                      # file offset of the image in the ELF files nso_to_elf writes
SFNT = (b'\x00\x01\x00\x00', b'OTTO', b'ttcf', b'true')
BFTTF_PLAIN_MAGIC = 0x7f9a0218


def nso_info(d):
    """Segments (decompressed), build id and integrity of an NSO."""
    import lz4.block
    if d[:4] != b'NSO0':
        raise ValueError('not an NSO executable')
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
    """NSO -> ELF64 (sections + program headers, image at file offset 0x1000)."""
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


def sdk_version(module):
    """The NintendoSDK a module was built with, from the version its own middleware tags carry
    ("NintendoSDK_<library>-<major>_<minor>_<micro>-Release"), as "7.3.2.0"; None when it carries none."""
    found = {m.groups() for m in re.finditer(rb'NintendoSDK_[A-Za-z0-9]+-(\d+)_(\d+)_(\d+)-Release', module)}
    if len(found) != 1:
        return None
    return '%s.%s.%s.0' % tuple(x.decode() for x in found.pop())


def npdm_program_id(d):
    """The program (title) id in an NPDM's ACI0 header."""
    if d[:4] != b'META':
        raise ValueError('not an NPDM file')
    aci = struct.unpack_from('<I', d, 0x70)[0]
    if d[aci:aci + 4] != b'ACI0':
        raise ValueError('NPDM without an ACI0 header')
    return struct.unpack_from('<Q', d, aci + 0x10)[0]


def nacp_info(d):
    """Name, display version and save sizes from a control.nacp."""
    names = []
    for i in range(16):
        n = d[i * 0x300:i * 0x300 + 0x200].split(b'\0')[0].decode('utf8', 'replace')
        if n:
            names.append(n)
    return {'name': names[0] if names else '',
            'display_version': d[0x3060:0x3070].split(b'\0')[0].decode('ascii', 'replace'),
            'user_save_size': struct.unpack_from('<q', d, 0x3080)[0],
            'user_save_journal': struct.unpack_from('<q', d, 0x3088)[0]}


def decode_bfttf(data):
    """The TrueType / OpenType file inside the console's .bfttf wrapper (every big-endian word XORed with a value that
    follows from the known first word); b'' when the result is not a font."""
    words = struct.unpack('>%dI' % (len(data) // 4), data[:len(data) // 4 * 4])
    key = words[0] ^ BFTTF_PLAIN_MAGIC
    size = words[1] ^ key
    out = struct.pack('>%dI' % (len(words) - 2), *[w ^ key for w in words[2:]])
    out = out[:size] if size <= len(out) else b''
    return out if out[:4] in SFNT else b''
