"""Name the lifted game's functions in a run's profile (SMO_HOSTPROF lines, "guest f_<address>").

    python tools/dev/profnames.py build/run/<name>.log [--functions build/functions/main_functions.tsv]

Prints the log's hostprof: lines with each "guest f_<address>" followed by the function's name from the function list
(demangled with llvm-cxxfilt when it is found next to clang, e.g. C:/llvm-9.0.0/bin).
"""
import argparse
import os
import re
import shutil
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, 'tools'))
import target as T  # noqa: E402  (SMO_TARGET: build/ or build/<update version>/)
MAIN_BASE = 0x7100000000


def load_names(tsv):
    names = {}
    for line in open(tsv, encoding='utf-8', errors='replace'):
        f = line.rstrip('\n').split('\t')
        name = next((x for x in (f[5:6] + f[6:7] + f[8:9]) if x), '')   # symbol, decomp, RTTI (1.4.1)
        if not name:
            continue
        try:
            names[MAIN_BASE + int(f[0], 16)] = name.lstrip('#').split('|')[0]
        except ValueError:
            pass
    return names


def demangle(mangled):
    tool = shutil.which('llvm-cxxfilt') or next((p for p in (r'C:\llvm-9.0.0\bin\llvm-cxxfilt.exe',) if os.path.exists(p)), None)
    if not tool or not mangled:
        return {}
    out = subprocess.run([tool], input='\n'.join(mangled), capture_output=True, text=True).stdout.split('\n')
    return dict(zip(mangled, out))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('log')
    ap.add_argument('--functions', default=T.build('functions', 'main_functions.tsv'))
    ap.add_argument('--width', type=int, default=110)
    a = ap.parse_args()
    lines = [l.rstrip('\n') for l in open(a.log, encoding='utf-8', errors='replace') if l.startswith('hostprof:')]
    names = load_names(a.functions)
    used = {}
    for l in lines:
        for g in re.findall(r'guest f_([0-9a-f]+)', l):
            n = names.get(int(g, 16))
            if n:
                used[g] = n
    dem = demangle(sorted(set(used.values())))
    for l in lines:
        m = re.search(r'guest f_([0-9a-f]+)', l)
        if m and m.group(1) in used:
            n = used[m.group(1)]
            l = '%s  %s' % (l, (dem.get(n) or n)[:a.width])
        print(l)


if __name__ == '__main__':
    main()
