"""Import your decrypted dump of Super Mario Odyssey 1.4.1 into build/.

    python tools/extract/extract.py --dump <folder> [--build <build folder, default build/>]

<folder> holds what you dumped from your own console, already decrypted by the console (docs/building.md):
    exefs/          the program's ExeFS with the 1.4.1 update: main, main.npdm, rtld, sdk, subsdk*
    romfs/          the game's RomFS with the 1.4.1 update applied
    control.nacp    the update's control data
    manual/         optional: the Help pages (the RomFS of the game's HtmlDocument content: html-document/...)
    fonts/          optional: the console's shared fonts, as .bfttf files the way the console stores them, or .ttf
Each piece is checked: the program id in main.npdm, the NintendoSDK version main carries (7.3.2 for 1.4.1), the display
version in control.nacp (1.4.1, or 1.3.0: the earlier update with the same program), and the RomFS and Help folders by
what they contain.

Writes, all under build/ (ignored by git):
    dump/update/exefs/<name>, elf/<name>.elf, control.nacp   the program; its modules converted to ELF
    dump/version.json                                        what was found: versions, build ids, SHA-256 of modules
    romfs, manual                                            directory junctions to your dump's folders (read in place)
    sysdata/fonts/<name>.ttf                                 the shared fonts, unwrapped (when given)
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nso  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, 'tools'))
import target as T  # noqa: E402

PROGRAM_ID = 0x0100000000010000                   # Super Mario Odyssey
SDK = '7.3.2.0'                                   # the NintendoSDK the 1.4.1 program was built with


class DumpError(Exception):
    pass


def find(dump, *names):
    for n in names:
        p = os.path.join(dump, n)
        if os.path.exists(p):
            return p
    return None


def read_exefs(path, need):
    """{name: bytes} of an ExeFS folder, checked to be the 1.4.1 program."""
    files = {n: open(os.path.join(path, n), 'rb').read() for n in sorted(os.listdir(path))
             if os.path.isfile(os.path.join(path, n))}
    missing = [n for n in need if n not in files]
    if missing:
        raise DumpError('%s: %s missing' % (path, ', '.join(missing)))
    if 'main.npdm' in files and nso.npdm_program_id(files['main.npdm']) != PROGRAM_ID:
        raise DumpError('%s: main.npdm is not Super Mario Odyssey\'s (program id %016x)'
                        % (path, nso.npdm_program_id(files['main.npdm'])))
    elf, _ = nso.nso_to_elf(files['main'])
    sdk = nso.sdk_version(elf)
    if sdk != SDK:
        raise DumpError('%s: its main was built with NintendoSDK %s, the 1.4.1 update\'s with %s: is this the ExeFS '
                        'with the 1.4.1 update?' % (path, sdk, SDK))
    return files


def write_exefs(files, dst):
    """ExeFS files as given and every NSO converted to ELF; {name: facts} for version.json."""
    for sub in ('exefs', 'elf'):
        os.makedirs(os.path.join(dst, sub), exist_ok=True)
    modules = {}
    for name, d in files.items():
        open(os.path.join(dst, 'exefs', name), 'wb').write(d)
        m = {'size': len(d), 'sha256': hashlib.sha256(d).hexdigest()}
        if d[:4] == b'NSO0':
            elf, meta = nso.nso_to_elf(d)
            open(os.path.join(dst, 'elf', name + '.elf'), 'wb').write(elf)
            m.update({'build_id': meta['build_id'], 'segments_sha_ok': meta['segments_sha_ok'],
                      'text': ['0x%x' % x for x in meta['text']], 'rodata': ['0x%x' % x for x in meta['rodata']],
                      'data': ['0x%x' % x for x in meta['data']], 'bss': ['0x%x' % x for x in meta['bss']],
                      'nsym': meta['nsym'], 'elf_sha256': hashlib.sha256(elf).hexdigest()})
            if not meta['segments_sha_ok']:
                raise DumpError('%s: its segments do not match the hashes in its own header: a damaged dump' % name)
        modules[name] = m
        print('  %-10s %10d bytes  %s' % (name, len(d), m.get('build_id', '')[:40]))
    return modules


def link_folder(link, target):
    """build/<link> becomes a directory junction to the dump's folder (no copy). A junction already there is replaced;
    a real folder is left alone (it was copied there before): delete it to read the dump in place."""
    if os.path.isdir(link) and not os.path.isjunction(link):
        if any(os.scandir(link)):
            print('  %s is a folder of its own, not a link to your dump: left as it is (delete it to use %s)' % (link, target))
            return
        os.rmdir(link)
    elif os.path.isjunction(link):
        os.rmdir(link)                                 # removes the junction only, never what it points to
    os.makedirs(os.path.dirname(link), exist_ok=True)
    try:
        import _winapi
        _winapi.CreateJunction(os.path.abspath(target), link)
    except (ImportError, OSError):
        subprocess.run(['cmd', '/c', 'mklink', '/J', link, os.path.abspath(target)], check=True, capture_output=True)
    print('  %s -> %s' % (link, target))


def import_fonts(src, dst):
    os.makedirs(dst, exist_ok=True)
    n = 0
    for name in sorted(os.listdir(src)):
        raw = open(os.path.join(src, name), 'rb').read()
        if name.lower().endswith('.bfttf'):
            font = nso.decode_bfttf(raw)
        elif name.lower().endswith(('.ttf', '.otf')):
            font = raw if raw[:4] in nso.SFNT else b''
        else:
            continue
        if not font:
            print('  %s: not a font' % name)
            continue
        open(os.path.join(dst, os.path.splitext(name)[0] + '.ttf'), 'wb').write(font)
        n += 1
    print('  %d fonts -> %s' % (n, dst))
    return n


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--dump', required=True, help='folder of your decrypted dump (see the layout above)')
    ap.add_argument('--build', default=T.BUILD, help='build folder to import into (default build/)')
    a = ap.parse_args()
    dump = os.path.abspath(a.dump)
    if not os.path.isdir(dump):
        raise SystemExit('%s: no such folder' % dump)
    try:
        exefs = find(dump, 'exefs')
        romfs = find(dump, 'romfs')
        nacp = find(dump, 'control.nacp')
        for what, p in (('exefs/ (the program with the 1.4.1 update)', exefs), ('romfs/ (the RomFS with the update applied)', romfs),
                        ('control.nacp (the update\'s)', nacp)):
            if not p:
                raise DumpError('%s: %s is missing (see docs/building.md for what to dump)' % (dump, what))
        info = nso.nacp_info(open(nacp, 'rb').read())
        if info['display_version'] not in T.DISPLAY_VERSIONS:
            raise DumpError('%s: version %s, this project builds %s (the control data of update %s)'
                            % (nacp, info['display_version'] or '(none)', T.VERSION, ' or '.join(T.DISPLAY_VERSIONS)))
        if not all(os.path.isdir(os.path.join(romfs, d)) for d in ('SystemData', 'StageData', 'ObjectData')):
            raise DumpError('%s: not the game\'s RomFS (no SystemData, StageData or ObjectData folder)' % romfs)
        upd_files = read_exefs(exefs, ('main', 'main.npdm', 'sdk', 'rtld'))
    except DumpError as e:
        raise SystemExit('dump: %s' % e)

    out = os.path.join(a.build, 'dump')
    print('program (%s, version %s): %s' % (info['name'], info['display_version'], exefs))
    shutil.rmtree(os.path.join(out, 'update'), ignore_errors=True)
    result = {'update': {'program_id': '%016x' % PROGRAM_ID, 'program_sdk_version': SDK, 'nacp': info,
                         'modules': write_exefs(upd_files, os.path.join(out, 'update'))}}
    shutil.copyfile(nacp, os.path.join(out, 'update', 'control.nacp'))
    shutil.rmtree(os.path.join(out, 'base'), ignore_errors=True)     # the base game's main older builds kept (unused)
    with open(os.path.join(out, 'version.json'), 'w') as f:
        json.dump(result, f, indent=1)
    print('wrote', os.path.join(out, 'version.json'))

    print('read in place:')
    link_folder(os.path.join(a.build, 'romfs'), romfs)
    manual = find(dump, 'manual')
    if manual and os.path.isdir(os.path.join(manual, 'html-document')):
        link_folder(os.path.join(a.build, 'manual'), manual)
    else:
        print('  no manual/ with html-document/ in it: the game\'s Help pages will be missing (optional)')
    fonts = find(dump, 'fonts')
    if fonts:
        import_fonts(fonts, os.path.join(a.build, 'sysdata', 'fonts'))
    else:
        print('  no fonts/: the game gets free fonts in place of the console\'s shared fonts (optional)')


if __name__ == '__main__':
    main()
