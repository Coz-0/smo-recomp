"""Extract the RomFS of the BASE package (1.0.0, no update) into build/romfs, for the runtime's nn::fs (MountRom).

    python tools/extract/romfs.py (--dir <folder with your NSPs> | --base <nsp>) [--keys <prod.keys>] [--out build/romfs] [--list]
    python tools/extract/romfs.py ... --manual [--out build/manual]

Reads the Program NCA straight from your NSP (decrypted in memory with your own keys; see switchfs.py). --manual
extracts the base package's Manual NCAs instead: the offline HTML document (html-document/: the game's Help, shown by
the runtime's web applet) and the legal information pages.
The output is derived from the dump and stays under build/ (ignored by git). Files already present with the right
size are skipped, so an interrupted run resumes.
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import switchfs as S

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dir', help='folder searched (recursively) for the base package, [0100000000010000] in its name')
    ap.add_argument('--base')
    ap.add_argument('--keys')
    ap.add_argument('--out')
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--manual', action='store_true', help="the Manual NCAs (offline HTML document, legal information)")
    a = ap.parse_args()
    if not a.out:
        a.out = os.path.join(REPO, 'build', 'manual' if a.manual else 'romfs')
    if not a.base and not a.dir:
        ap.error('give --dir (a folder holding your base NSP) or --base <nsp>')
    if not a.base:
        for root, _, files in os.walk(a.dir):
            for n in files:
                if n.lower().endswith('.nsp') and '[0100000000010000]' in n:
                    a.base = os.path.join(root, n)
    pkg = S.Package(a.base, S.Keys(a.keys))
    ncas = pkg.by_type('Manual') if a.manual else pkg.by_type('Program')[:1]
    if not ncas:
        raise SystemExit('no %s NCA in %s' % ('Manual' if a.manual else 'Program', a.base))
    for nca in ncas:
        extract_nca(nca, a.out, a.list)


def extract_nca(nca, out, listing):
    read, base = nca.romfs()
    entries = sorted(S.romfs_walk(read, base))
    total = sum(e[2] for e in entries)
    print('%s: %d files, %.2f GB' % (nca.name, len(entries), total / 2 ** 30))
    if listing:
        for p, _, size in entries:
            print('%12d  %s' % (size, p))
        return
    t0 = time.time()
    done = skipped = 0
    for path, off, size in entries:
        target = os.path.join(out, *path.strip('/').split('/'))
        if os.path.exists(target) and os.path.getsize(target) == size:
            skipped += 1
            done += size
            continue
        os.makedirs(os.path.dirname(target), exist_ok=True)
        tmp = target + '.part'
        with open(tmp, 'wb') as f:
            pos = 0
            while pos < size:
                n = min(64 << 20, size - pos)
                f.write(read(off + pos, n))
                pos += n
        os.replace(tmp, target)
        done += size
    print('extracted %d files (%d already present), %.2f GB, in %.0f s -> %s'
          % (len(entries) - skipped, skipped, done / 2 ** 30, time.time() - t0, out))

if __name__ == '__main__':
    main()
