"""Repair C string literals that a shell heredoc split across two lines (a "\\n" inside the literal became a real newline).

    python tools/dev/fix_split_strings.py <file.c> [...]

A line whose last double quote opens a string that is never closed, followed by a line that starts with `"`, is joined back
with the two characters backslash-n. Prints what it joined.
"""
import sys


def open_string(line):
    q, i = False, 0
    while i < len(line):
        ch = line[i]
        if ch == '\\' and q:
            i += 2
            continue
        if ch == '"':
            q = not q
        elif ch == "'" and not q:                 # skip a character literal such as '"'
            j = line.find("'", i + 1)
            if j > 0:
                i = j
        i += 1
    return q


for path in sys.argv[1:]:
    lines = open(path, encoding='utf-8').read().split('\n')
    out, n, i = [], 0, 0
    while i < len(lines):
        ln = lines[i]
        if i + 1 < len(lines) and open_string(ln) and lines[i + 1].startswith('"'):
            out.append(ln + '\\n' + lines[i + 1])
            print('%s:%d joined' % (path, i + 1))
            n += 1
            i += 2
            continue
        out.append(ln)
        i += 1
    if n:
        open(path, 'w', encoding='utf-8', newline='\n').write('\n'.join(out))
