"""Name the host function at an offset of an exe, from the MSVC-format linker map build_exe.py writes.

    python tools/lifter/mapaddr.py <exe.map> <offset hex> [<offset hex> ...]

Offsets are what a crash report prints ("in smo_test.exe at offset 0x...": relative to the module base).
"""
import re
import sys


def load(path):
    base, syms = 0x140000000, []
    for line in open(path, encoding='latin-1'):
        m = re.match(r'\s*Preferred load address is ([0-9a-fA-F]+)', line)
        if m:
            base = int(m.group(1), 16)
        m = re.match(r'\s*[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})\s+[f ]\s*(\S*)', line)
        if m:
            syms.append((int(m.group(2), 16), m.group(1), m.group(3)))
    syms.sort()
    return base, syms


def main():
    base, syms = load(sys.argv[1])
    for off in sys.argv[2:]:
        a = base + int(off, 16)
        best = None
        for s in syms:
            if s[0] <= a:
                best = s
            else:
                break
        if best:
            print('0x%s -> %s+0x%x  (%s)' % (off, best[1], a - best[0], best[2]))
        else:
            print('0x%s -> ?' % off)


if __name__ == '__main__':
    main()
