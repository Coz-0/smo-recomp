"""Extract a RomFS of your game into build/, for the runtime's nn::fs (MountRom).

    python tools/extract/romfs.py (--dir <folder with your NSPs> | --base <nsp>) [--keys <prod.keys>] [--out <folder>]
                                  [--manual] [--update [--update-nsp <nsp>]] [--verify] [--list]

Default: the RomFS of the BASE package (1.0.0, [0100000000010000] in its name) -> build/romfs.
--manual: the Manual NCAs instead: the offline HTML document (html-document/: the game's Help, shown by the runtime's
web applet) and the legal information pages -> build/manual.
--update: with your update package applied ([0100000000010800] in its name), whose RomFS sections are patches of the
base's (BKTR, see switchfs.PatchedSection) -> build/<its version>/romfs (or manual). A file byte-identical to the
base's copy already extracted (build/romfs, build/manual) is hard-linked to it instead of written again. The run ends
with what the update changed, added and removed, listed in build/<version>/romfs_changes.txt (manual_changes.txt).
--verify: check the hash tree (IVFC) of each RomFS before extracting it: every block of every level, up to the master
hash in the section header; a wrong block stops the run. --list: list the files instead of extracting them.

Reads the NCAs straight from your NSPs (decrypted in memory with your own keys; see switchfs.py). The output is
derived from the dump and stays under build/ (ignored by git). Files already present with the right size are skipped,
so an interrupted run resumes.
"""
import argparse
import os
import shutil
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import switchfs as S

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BUILD = os.path.join(REPO, 'build')
BASE_ID, UPDATE_ID = '0100000000010000', '0100000000010800'
CHUNK = 64 << 20


def find_nsp(folder, title_id):
    for root, _, files in os.walk(folder):
        for n in sorted(files):
            if n.lower().endswith('.nsp') and '[%s]' % title_id in n.lower():
                return os.path.join(root, n)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dir', help='folder searched (recursively) for your packages: [%s] in the name of the base, '
                                  '[%s] in the name of the update' % (BASE_ID, UPDATE_ID))
    ap.add_argument('--base')
    ap.add_argument('--update-nsp')
    ap.add_argument('--keys')
    ap.add_argument('--out')
    ap.add_argument('--manual', action='store_true', help="the Manual NCAs (offline HTML document, legal information)")
    ap.add_argument('--update', action='store_true', help='apply your update package to the base')
    ap.add_argument('--verify', action='store_true', help='check the hash tree of each RomFS before extracting it')
    ap.add_argument('--list', action='store_true')
    a = ap.parse_args()
    a.base = a.base or (a.dir and find_nsp(a.dir, BASE_ID))
    if not a.base:
        ap.error('give --dir (a folder holding your base NSP) or --base <nsp>')
    keys = S.Keys(a.keys)
    base = S.Package(a.base, keys)
    what = 'manual' if a.manual else 'romfs'

    if not a.update:
        ncas = base.by_type('Manual') if a.manual else base.by_type('Program')[:1]
        if not ncas:
            raise SystemExit('no %s NCA in %s' % ('Manual' if a.manual else 'Program', a.base))
        for nca in ncas:
            read, off = nca.romfs()
            run(nca.name, read, off, nca.romfs_section()['fs'], a.out or os.path.join(BUILD, what), a)
        return

    a.update_nsp = a.update_nsp or (a.dir and find_nsp(a.dir, UPDATE_ID))
    if not a.update_nsp:
        ap.error('--update needs your update package: give --dir (a folder holding it) or --update-nsp <nsp>')
    upd = S.Package(a.update_nsp, keys)
    version = S.nacp_info(upd.nacp())['display_version']
    out = a.out or os.path.join(BUILD, version, what)
    against = os.path.join(BUILD, what) if os.path.isdir(os.path.join(BUILD, what)) else None
    base_kinds, upd_kinds = base.contents(), upd.contents()
    if a.manual:                         # HtmlDocument patches the base's HtmlDocument, LegalInformation the base's
        pairs = [(m, next((b for b in base.by_type('Manual') if base_kinds.get(b.name[:32]) == upd_kinds.get(m.name[:32])),
                          None)) for m in upd.by_type('Manual')]
    else:
        pairs = [(upd.by_type('Program')[0], base.by_type('Program')[0])]
    changes = Changes()
    for u, b in pairs:
        if b is not None:
            changes.base_files.update((p, size) for p, _, size in S.romfs_walk(*b.romfs()))
        sec = u.romfs_section()
        if sec['crypto'] == S.CRYPTO_BKTR:
            if b is None:
                raise SystemExit('%s patches an NCA that %s does not have' % (u.name, a.base))
            p = S.PatchedSection(u, sec, b, next(s for s in b.sections if s['index'] == sec['index']))
            read, off = p.romfs()
            run('%s %s: %s patched by %s' % (version, what, b.name, u.name), read, off, sec['fs'], out, a, against,
                p.from_update, changes)
        else:
            read, off = u.romfs()
            run('%s %s: %s (complete)' % (version, what, u.name), read, off, sec['fs'], out, a, against,
                lambda o, n: n, changes)
    if not a.list:
        changes.report(against, os.path.join(os.path.dirname(out), what + '_changes.txt'), version)
        if changes.rows and not a.verify:
            print('  (not proven: run with --verify to check the patched data against the update\'s hash tree)')


def run(label, read, off, fs, out, a, against=None, from_update=None, changes=None):
    entries = sorted(S.romfs_walk(read, off))
    print('%s: %d files, %.2f GB' % (label, len(entries), sum(e[2] for e in entries) / 2 ** 30), flush=True)
    if a.verify:
        t0 = time.time()
        levels = S.ivfc_verify(read, fs)
        for level, blocks, wrong, short in levels:
            print('  hash tree level %d: %d blocks, %d wrong%s'
                  % (level, blocks, wrong, ', short last block matched %s' % short if short else ''))
        print('  hash tree checked in %.0f s' % (time.time() - t0), flush=True)
        if any(wrong for _, _, wrong, _ in levels):
            raise SystemExit('%s: the hash tree does not match the data: stopped' % label)
    if a.list:
        for path, _, size in entries:
            print('%12d  %s' % (size, path))
        return
    extract(entries, read, out, against, from_update, changes)


def extract(entries, read, out, against, from_update, changes):
    t0 = time.time()
    written = linked = present = 0
    for path, off, size in entries:
        parts = path.strip('/').split('/')
        target = os.path.join(out, *parts)
        ref = os.path.join(against, *parts) if against else None
        if ref and not os.path.isfile(ref):
            ref = None
        if os.path.exists(target) and os.path.getsize(target) == size:
            present += 1
            same = ref is not None and (os.path.samefile(ref, target) or same_files(ref, target))
        else:
            os.makedirs(os.path.dirname(target), exist_ok=True)
            if os.path.exists(target):
                os.remove(target)
            same = ref is not None and os.path.getsize(ref) == size and same_data(read, off, size, ref)
            if same:
                link(ref, target)
                linked += 1
            else:
                write(read, off, size, target)
                written += 1
        if changes is not None:
            changes.add(path, size, ref is not None, same, from_update(off, size))
    print('extracted %d files (%d written, %d hard-linked to the same file of %s, %d already present), %.2f GB, in %.0f s'
          ' -> %s' % (len(entries), written, linked, against, present, sum(e[2] for e in entries) / 2 ** 30,
                      time.time() - t0, out), flush=True)


def write(read, off, size, target):
    tmp = target + '.part'
    with open(tmp, 'wb') as f:
        pos = 0
        while pos < size:
            n = min(CHUNK, size - pos)
            f.write(read(off + pos, n))
            pos += n
    os.replace(tmp, target)


def link(ref, target):
    """The base's copy under a second name (no second copy on disk); a plain copy where hard links are not possible."""
    try:
        os.link(ref, target)
    except OSError:
        shutil.copyfile(ref, target)


def same_data(read, off, size, ref):
    with open(ref, 'rb') as f:
        pos = 0
        while pos < size:
            n = min(CHUNK, size - pos)
            if read(off + pos, n) != f.read(n):
                return False
            pos += n
    return True


def same_files(a, b):
    with open(a, 'rb') as fa, open(b, 'rb') as fb:
        while True:
            x, y = fa.read(CHUNK), fb.read(CHUNK)
            if x != y:
                return False
            if not x:
                return True


class Changes:
    """What the update did to each file: compared with the base's extracted copy of the same path, and with the base
    package's own file list (the extracted folder can hold files the runtime wrote there)."""

    def __init__(self):
        self.rows = []                   # (kind, path, size, bytes taken from the update's own data)
        self.base_files = set()          # (path, size) of the base NCAs the update applies to

    def add(self, path, size, in_base, same, from_update):
        self.rows.append(('unchanged' if same else 'changed' if in_base else 'added', path, size, from_update))

    def report(self, against, listing, version):
        paths = {r[1] for r in self.rows}
        removed = [('removed', p, size, 0) for p, size in self.base_files if p not in paths]
        kinds = {k: [r for r in self.rows + removed if r[0] == k] for k in ('unchanged', 'changed', 'added', 'removed')}
        print('%s against %s: %d unchanged (byte-identical), %d changed, %d added, %d removed'
              % (version, against, *(len(kinds[k]) for k in ('unchanged', 'changed', 'added', 'removed'))))
        print('  update data: %.1f MB in %d files; changed or added files made only of base data from another place: %d;'
              ' unchanged files the update stored again: %d'
              % (sum(r[3] for r in self.rows) / 2 ** 20, sum(1 for r in self.rows if r[3]),
                 sum(1 for r in kinds['changed'] + kinds['added'] if r[3] == 0), sum(1 for r in kinds['unchanged'] if r[3])))
        with open(listing, 'w', encoding='utf-8', newline='\n') as f:
            f.write('# kind\tsize\tbytes from the update (0: base data from another place)\tpath\n')
            for k in ('changed', 'added', 'removed'):
                for r in sorted(kinds[k], key=lambda r: r[1]):
                    f.write('%s\t%d\t%d\t%s\n' % (k, r[2], r[3], r[1]))
        print('  listed in %s' % listing)


if __name__ == '__main__':
    main()
