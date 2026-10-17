"""Run the built game for a while and summarise what happened.

    python scripts/run.py <name> [--seconds 30] [--exe smo_test.exe] [--env NAME=VALUE ...] [--full]
    python scripts/run.py <name> --exe smo_native.exe --seconds 0 [--userdata <folder>]      play: window, sound, no time limit
                          [--seq <file.seq>] [--userdata <folder>]

Copies build/bin/smo_native.exe (+ its .map) to the test name (only smo_native.exe / smo_user.exe open a visible window
and play sound; SMO_SHOW=1 shows a test exe), runs it from build/bin with the converted dump's main.elf and the time
limit, writes everything to build/run/<name>.log, then prints: the boot phases reached, the unimplemented imports
(name and call count), the do-nothing audit line, frames presented, and a crash with its host frames named from the
linker map. --full prints the whole log instead.

--seq plays an input sequence file (SMO_SEQ, runtime/nnhid.c; one or more steps per line, '#' starts a comment), e.g.
tests/cap_kingdom.seq. --userdata gives the game its own save folder (SMO_USERDATA; created if missing): a new, empty
folder starts a new game, a copy of an earlier run's folder continues from there. Without it saves go to build/userdata.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(REPO, 'build', 'bin')
RUN = os.path.join(REPO, 'build', 'run')
sys.path.insert(0, os.path.join(REPO, 'tools', 'lifter'))
import mapaddr  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('name')
    ap.add_argument('--seconds', type=int, default=30)
    ap.add_argument('--exe', default='smo_test.exe')
    ap.add_argument('--env', action='append', default=[])
    ap.add_argument('--full', action='store_true')
    ap.add_argument('--seq', help='input sequence file (SMO_SEQ)')
    ap.add_argument('--userdata', help='save folder for this run (SMO_USERDATA)')
    a = ap.parse_args()
    os.makedirs(RUN, exist_ok=True)
    os.makedirs(os.path.join(REPO, 'build', 'userdata'), exist_ok=True)
    if a.seq:
        steps = [line.split('#', 1)[0].strip() for line in open(a.seq, encoding='utf-8')]
        a.env.append('SMO_SEQ=' + ' '.join(s for s in steps if s))
    if a.userdata:
        os.makedirs(a.userdata, exist_ok=True)
        a.env.append('SMO_USERDATA=' + os.path.abspath(a.userdata))
    src = os.path.join(BIN, 'smo_native.exe')
    exe = os.path.join(BIN, a.exe)
    if a.exe != 'smo_native.exe':
        shutil.copy2(src, exe)
        shutil.copy2(src[:-4] + '.map', exe[:-4] + '.map')
    env = dict(os.environ)
    for kv in a.env:
        k, v = kv.split('=', 1)
        env[k] = v
    log = os.path.join(RUN, a.name + '.log')
    with open(log, 'w', encoding='utf-8', errors='replace') as f:
        try:
            r = subprocess.run([exe, r'..\dump\base\elf\main.elf', str(max(a.seconds, 0))], cwd=BIN, stdout=f, stderr=subprocess.STDOUT,
                               env=env, timeout=a.seconds + 120 if a.seconds > 0 else None)      # 0: no time limit, until the window is closed
            code = r.returncode
        except subprocess.TimeoutExpired:
            code = 'timeout (killed)'
    text = open(log, encoding='utf-8', errors='replace').read()
    if a.full:
        print(text)
        return
    lines = text.split('\n')
    print('log: %s   exit code: %s   lines: %d' % (log, code, len(lines)))
    for l in lines:
        if l.startswith(('[phase', 'fake audit', 'display: ', 'nnMain returned', 'still running', 'GUEST TRAP', 'UNTRANSLATED',
                         'STRICT', 'memory heap', 'imports reached', 'main thread: last')) or 'frames presented' in l:
            print('  ' + l[:220])
    if 'unimplemented imports reached:' in text:
        part = text.split('unimplemented imports reached:', 1)[1].split('\n\n', 1)[0]
        print('unimplemented imports reached (calls, name):')
        for l in part.strip('\n').split('\n'):
            if l.strip():
                print('  ' + l.strip())
    m = re.search(r'HOST EXCEPTION.*?(?=\nimports reached|\Z)', text, re.S)
    if m:
        block = m.group(0)
        print('\n'.join('  ' + l for l in block.split('\n')[:4]))
        base, syms = mapaddr.load(exe[:-4] + '.map')
        for mod, off in re.findall(r'^\s+(\S+?)\+0x([0-9a-f]+)$', block, re.M):
            name = ''
            if mod.lower() == a.exe.lower():
                addr = base + int(off, 16)
                best = None
                for s in syms:
                    if s[0] <= addr:
                        best = s
                    else:
                        break
                name = '%s+0x%x' % (best[1], addr - best[0]) if best else '?'
            print('    %s+0x%s %s' % (mod, off, name))
        for l in block.split('\n'):
            if l.strip().startswith('f_') or 'last guest function' in l:
                print('  ' + l.strip())


if __name__ == '__main__':
    main()
