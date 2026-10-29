"""Offline check of scripts/launcher.py's graphics.ini handling (no game, no build needed).

    python tests/check_launcher.py

Checks: the keys read from the runtime's default text (runtime/gfx/gfx_options.c) are the keys gfx_options.c parses;
a file written without one is the runtime's default text with only the changed values different (same line ends);
in an existing file only the 'key = value' lines change, comments and unknown keys stay, a new key is appended.
"""
import os
import re
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, 'scripts'))
import launcher as L  # noqa: E402

failed = 0


def check(what, ok):
    global failed
    print('  %-4s %s' % ('ok' if ok else 'FAIL', what))
    failed += not ok


text = L.default_ini()
keys = L.ini_keys(text)
with open(L.GFX_SOURCE, encoding='utf-8') as f:
    src = f.read()
parsed = set(re.findall(r'_stricmp\(key, "(\w+)"\)', src))      # the reader's key tests (gfx_options.c)
check('%d keys in the default text, every one parsed by gfx_options.c' % len(keys),
      bool(keys) and {k for k, _, _, _ in keys} <= parsed)
check('every key with choices has its default among them', all(d in c for _, d, c, _ in keys if c))

with tempfile.TemporaryDirectory() as tmp:
    path = os.path.join(tmp, 'config', 'graphics.ini')
    first, last = keys[0], keys[-1]
    new_first = next(c for c in (first[2] or ['x']) if c != first[1])
    L.write_values(path, {first[0]: new_first})
    with open(path, encoding='utf-8', newline='') as f:
        written = f.read()
    expect = re.sub(r'(?m)^%s = .*?(\r?)$' % first[0], lambda m: '%s = %s%s' % (first[0], new_first, m.group(1)), text, count=1)
    check('no file: the default text with only %s changed' % first[0], written == expect)

    with open(path, 'a', encoding='utf-8', newline='') as f:
        f.write('; a comment of mine\r\nsome_other_key = 7\r\n')
    L.write_values(path, {last[0]: 'x', 'brand_new': 'y'})
    v = L.read_values(path)
    with open(path, encoding='utf-8', newline='') as f:
        again = f.read()
    check('existing file: the value set, others kept', v[last[0]] == 'x' and v[first[0]] == new_first and v['some_other_key'] == '7')
    check('existing file: comments kept, new key appended', '; a comment of mine' in again and v.get('brand_new') == 'y')
    check('existing file: line ends kept (CRLF only)', '\n' not in again.replace('\r\n', ''))

print('RESULT: %s' % ('PASS' if not failed else 'FAIL (%d)' % failed))
sys.exit(1 if failed else 0)
