"""Extraction: ExeFS modules, ELF conversions, NACP and the exact version of your own SMO packages.

    python tools/extract/extract.py (--dir <folder> | --base <base.nsp> [--update <update.nsp>]) [--keys <prod.keys>] [--out build/dump]
                                    [--decomp <OdysseyDecomp clone, optional>]

Writes, for each package given, into <out>/<base|update>/:
    exefs/<name>          the ExeFS files exactly as stored (main, sdk, subsdk*, rtld, main.npdm)
    elf/<name>.elf        every NSO converted to ELF (image at file offset 0x1000)
    control.nacp
and <out>/version.json: title ids, versions (CNMT + NACP display version), SDK version, build ids, SHA-256 of every
module, segment-hash checks, and whether `main` equals OdysseyDecomp's 1.0 executable (hash read from the local
decomp clone's tools/setup.py, so nothing of the decomp is stored here).
The packages are only read. Everything written is derived from the dump and lives under build/ (ignored by git).
"""
import argparse
import hashlib
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import switchfs as S

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def decomp_hashes(decomp):
    p = os.path.join(decomp, 'tools', 'setup.py')
    if not os.path.isfile(p):
        return {}
    src = open(p, encoding='utf-8').read()
    return {k: v for k, v in re.findall(r'(\w+_V10_\w*HASH|V10_ELF_HASH)\s*=\s*"([0-9a-f]{64})"', src)}


def extract(label, path, keys, out):
    pkg = S.Package(path, keys)
    dst = os.path.join(out, label)
    os.makedirs(os.path.join(dst, 'exefs'), exist_ok=True)
    os.makedirs(os.path.join(dst, 'elf'), exist_ok=True)
    info = {'package': os.path.basename(path), 'meta': pkg.meta(),
            'ncas': [{'name': n.name, 'type': S.CONTENT_TYPES.get(n.content_type, n.content_type),
                      'title_id': '%016x' % n.title_id, 'sdk_version': S.sdk_version_str(n.sdk_version),
                      'key_generation': n.key_gen, 'sections': [(s['fs_type'], s['crypto']) for s in n.sections]}
                     for n in pkg.ncas]}
    nacp = pkg.nacp()
    open(os.path.join(dst, 'control.nacp'), 'wb').write(nacp)
    info['nacp'] = S.nacp_info(nacp)
    prog = pkg.by_type('Program')[0]
    info['program_sdk_version'] = S.sdk_version_str(prog.sdk_version)
    exefs_sec = next((s for s in prog.sections if s['fs_type'] == 1), None)
    info['modules'] = {}
    if exefs_sec is None:
        print('  %s: program NCA has no ExeFS section' % label)
        return info
    pfs = prog.pfs0(exefs_sec)
    for name in sorted(pfs.files):
        d = pfs.read(name)
        open(os.path.join(dst, 'exefs', name), 'wb').write(d)
        m = {'size': len(d), 'sha256': hashlib.sha256(d).hexdigest()}
        if d[:4] == b'NSO0':
            elf, meta = S.nso_to_elf(d)
            open(os.path.join(dst, 'elf', name + '.elf'), 'wb').write(elf)
            m.update({'build_id': meta['build_id'], 'segments_sha_ok': meta['segments_sha_ok'],
                      'text': ['0x%x' % x for x in meta['text']], 'rodata': ['0x%x' % x for x in meta['rodata']],
                      'data': ['0x%x' % x for x in meta['data']], 'bss': ['0x%x' % x for x in meta['bss']],
                      'nsym': meta['nsym'], 'elf_sha256': hashlib.sha256(elf).hexdigest()})
        info['modules'][name] = m
        print('  %-10s %10d bytes  %s' % (name, len(d), m.get('build_id', '')[:40]))
    return info


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dir', help='folder searched (recursively) for the base [0100000000010000] and update '
                    '[0100000000010800] NSPs; avoids passing non-ASCII file names through a shell')
    ap.add_argument('--base')
    ap.add_argument('--update')
    ap.add_argument('--keys')
    ap.add_argument('--out', default=os.path.join(REPO, 'build', 'dump'))
    ap.add_argument('--decomp', default=os.path.join(os.path.dirname(REPO), 'ref', 'OdysseyDecomp'))
    a = ap.parse_args()
    if a.dir:
        for root, _, files in os.walk(a.dir):
            for n in files:
                if n.lower().endswith('.nsp'):
                    if '[0100000000010000]' in n and not a.base:
                        a.base = os.path.join(root, n)
                    elif '[0100000000010800]' in n and not a.update:
                        a.update = os.path.join(root, n)
    if not a.base:
        ap.error('no base NSP (give --base or --dir)')
    keys = S.Keys(a.keys)
    print('keys: %s' % keys.path)
    result = {}
    for label, path in (('base', a.base), ('update', a.update)):
        if path:
            print('%s: %s' % (label, path))
            result[label] = extract(label, path, keys, a.out)
            r = result[label]
            print('  title %s  version %d  display "%s"  sdk %s  name "%s"' % (
                r['meta']['title_id'], r['meta']['version'], r['nacp']['display_version'], r['program_sdk_version'],
                r['nacp']['name']))
    h = decomp_hashes(a.decomp)
    if h and 'base' in result and 'main' in result['base']['modules']:
        got = result['base']['modules']['main']['sha256']
        ok = got == h.get('COMPRESSED_V10_HASH')
        result['decomp_check'] = {'decomp_main_1_0_sha256': h.get('COMPRESSED_V10_HASH'), 'base_main_sha256': got,
                                  'match': ok}
        print('base main vs OdysseyDecomp 1.0 main: %s' % ('MATCH' if ok else 'DIFFERENT'))
    if h and 'update' in result and 'main' in result['update']['modules']:
        result.setdefault('decomp_check', {})['update_main_matches_1_0'] = (
            result['update']['modules']['main']['sha256'] == h.get('COMPRESSED_V10_HASH'))
    os.makedirs(a.out, exist_ok=True)
    with open(os.path.join(a.out, 'version.json'), 'w') as f:
        json.dump(result, f, indent=1)
    print('wrote', os.path.join(a.out, 'version.json'))


if __name__ == '__main__':
    main()
