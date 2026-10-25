"""Sanity check OF THE HARNESS (not of the lifter): does difftest.py notice wrong code?

    python tools/lifter/difftest_mutation.py

Copies build/difftest/difftest_gen.c, plants one deliberate bug in four tests (one per kind: an arithmetic operator,
a branch condition, a trap removed, an FP min/max swapped), builds that copy as build/difftest/mutant/difftest.exe and
runs `difftest.py check` against it. Passes only if exactly the four mutated tests are reported different.
The real tests, their generated code and the reference are not modified.
"""
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import recomp
import difftest

WORK = difftest.WORK
MUT = os.path.join(WORK, 'mutant')


def main():
    tests = json.load(open(os.path.join(WORK, 'tests.json')))
    src = open(os.path.join(WORK, 'difftest_gen.c')).read()
    picks = {}
    for i, t in enumerate(tests):
        st = t['stmt']
        if 'straight' not in picks and t['kind'] == 'straight' and t['key'].startswith('add x, x, #') and ' + ' in st:
            picks['straight'] = (i, lambda s: s.replace(' + ', ' - ', 1))
        elif 'branch' not in picks and t['kind'] == 'branch' and 'if (cond(' in st:
            picks['branch'] = (i, lambda s: s.replace('if (cond(', 'if (!cond(', 1))
        elif 'trap' not in picks and t['kind'] == 'trap':
            picks['trap'] = (i, lambda s: s.replace('guest_trap(', '(void)(', 1))
        elif ('special' not in picks and t['kind'] == 'special' and 'fp_maxmin' in st and t['frm'] >= 0
              and t['k'] == 2 + 3 * len(difftest.SPECIAL[t['fpmode']])):
            # operands +1.0 (table index 2) and -1.0 (index 3): max and min differ. (k = 0 would be +0 vs +0, where
            # swapping max and min is an EQUIVALENT mutant that no test can detect.)
            picks['special'] = (i, lambda s: re.sub(r'fp_maxmin(\d+)\(([^,]+), ([^,]+), (\d)', lambda m: 'fp_maxmin%s(%s, %s, %d' % (m.group(1), m.group(2), m.group(3), 1 - int(m.group(4))), s, count=1))
    lines = src.split('\n')
    for kind, (i, mutate) in picks.items():
        prefix = 'static void t_%d(Ctx *c) {' % i
        k = next(n for n, l in enumerate(lines) if l.startswith(prefix))
        new = mutate(lines[k])
        assert new != lines[k], kind
        lines[k] = new
        print('mutated %-8s test #%d  %s' % (kind, i, tests[i]['key']))
    os.makedirs(MUT, exist_ok=True)
    open(os.path.join(MUT, 'difftest_gen.c'), 'w').write('\n'.join(lines))
    exe = os.path.join(MUT, 'difftest' if difftest.MAC else 'difftest.exe')
    r = subprocess.run(difftest.host_build_cmd(os.path.join(MUT, 'difftest_gen.c'), exe), capture_output=True, text=True)
    if r.returncode:
        print(r.stderr[-2000:])
        sys.exit(2)
    env = dict(os.environ, DIFFTEST_EXE=exe)
    r = subprocess.run([sys.executable, os.path.join(HERE, 'difftest.py'), 'check'], capture_output=True, text=True,
                       env=env)
    print(r.stdout)
    bad = json.load(open(os.path.join(WORK, 'bad.json')))
    flagged = sorted(i for v in bad.values() for i in v)
    expected = sorted(i for i, _ in picks.values())
    ok = flagged == expected and r.returncode == 1
    print('harness %s: flagged %s, expected %s' % ('DETECTS the mutations' if ok else 'FAILED', flagged, expected))
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
