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
--target <version> runs the build of another game version (scripts/setup.py --target): build/<version>/ in place of
build/ for everything above.

A test copy first waits for one of SMO_MAX_GAMES places on this PC (default: one per 12 GB of memory, 2 on a 32 GB PC;
0 = no limit): a game in a stage takes 4-7 GB, and more test games than fit make the PC page, which slows every game
and can make graphics memory allocations fail. smo_native.exe and smo_user.exe never wait.
"""
import argparse
import ctypes
import os
import re
import shutil
import subprocess
import sys
import time
from ctypes import wintypes

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, 'tools', 'lifter'))
sys.path.insert(0, os.path.join(REPO, 'tools'))
import mapaddr  # noqa: E402

PLAYED = ('smo_native.exe', 'smo_user.exe')     # the copies a person plays: a window, sound, no waiting for a place


class _MemoryStatus(ctypes.Structure):
    _fields_ = [('dwLength', wintypes.DWORD), ('dwMemoryLoad', wintypes.DWORD)] + [
        (n, ctypes.c_ulonglong) for n in ('total_phys', 'avail_phys', 'total_page', 'avail_page', 'total_virtual',
                                          'avail_virtual', 'avail_extended')]


def max_games():
    """SMO_MAX_GAMES, or one test game per 12 GB of the PC's memory (at least one)."""
    if os.environ.get('SMO_MAX_GAMES'):
        return max(int(os.environ['SMO_MAX_GAMES']), 0)
    m = _MemoryStatus()
    m.dwLength = ctypes.sizeof(m)
    ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m))
    return max(int(m.total_phys / 2**30 / 12), 1)


def take_place(exe_name, poll=2.0, name='Local\\smo_test_game_%d'):
    """Waits until fewer than max_games() test games run on this PC and takes a place: the named mutex
    Local\\smo_test_game_<k>, held by this thread until release_place (a launcher that dies frees its place: the mutex
    is abandoned). Returns the mutex handle, or None for the played copies and with no limit."""
    n = max_games()
    if exe_name.lower() in PLAYED or n == 0:
        return None
    k32 = ctypes.WinDLL('kernel32', use_last_error=True)
    k32.CreateMutexW.restype = wintypes.HANDLE
    k32.CreateMutexW.argtypes = [ctypes.c_void_p, wintypes.BOOL, wintypes.LPCWSTR]
    k32.WaitForSingleObject.restype = wintypes.DWORD
    k32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    k32.CloseHandle.argtypes = [wintypes.HANDLE]
    waited = None
    while True:
        for k in range(n):
            h = k32.CreateMutexW(None, False, name % k)
            if not h:
                continue
            if k32.WaitForSingleObject(h, 0) in (0, 0x80):          # WAIT_OBJECT_0, WAIT_ABANDONED (its launcher died)
                if waited:
                    print('run.py: place %d of %d free after %.0f s' % (k, n, time.time() - waited))
                return h
            k32.CloseHandle(h)
        if waited is None:
            waited = time.time()
            print('run.py: %d test games are running (SMO_MAX_GAMES=%d): waiting for one to end' % (n, n))
            sys.stdout.flush()
        time.sleep(poll)


def release_place(h):
    if h:
        ctypes.windll.kernel32.ReleaseMutex(wintypes.HANDLE(h))
        ctypes.windll.kernel32.CloseHandle(wintypes.HANDLE(h))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('name')
    ap.add_argument('--seconds', type=int, default=30)
    ap.add_argument('--exe', default='smo_test.exe')
    ap.add_argument('--env', action='append', default=[])
    ap.add_argument('--full', action='store_true')
    ap.add_argument('--seq', help='input sequence file (SMO_SEQ)')
    ap.add_argument('--userdata', help='save folder for this run (SMO_USERDATA)')
    ap.add_argument('--target', help='game version of the build to run (default 1.0.0, or SMO_TARGET)')
    ap.add_argument('--timeout', type=int, help='seconds after which a game still running is killed (default --seconds + 120; '
                                                'a run of several stages, SMO_STAGE_LIST, has the time limit per stage)')
    a = ap.parse_args()
    if a.target:
        os.environ['SMO_TARGET'] = a.target
    import target as T
    BIN, RUN = T.build('bin'), T.build('run')
    os.makedirs(RUN, exist_ok=True)
    os.makedirs(T.build('userdata'), exist_ok=True)
    if a.seq:
        steps = [line.split('#', 1)[0].strip() for line in open(a.seq, encoding='utf-8')]
        a.env.append('SMO_SEQ=' + ' '.join(s for s in steps if s))
    if a.userdata:
        os.makedirs(a.userdata, exist_ok=True)
        a.env.append('SMO_USERDATA=' + os.path.abspath(a.userdata))
    src = os.path.join(BIN, 'smo_native.exe')
    exe = os.path.join(BIN, a.exe)
    place = take_place(a.exe)
    try:
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
                r = subprocess.run([exe, os.path.relpath(T.elf('main'), BIN), str(max(a.seconds, 0))], cwd=BIN, stdout=f,
                                   stderr=subprocess.STDOUT, env=env,      # 0: no time limit, until the window is closed
                                   timeout=a.timeout or (a.seconds + 120 if a.seconds > 0 else None))
                code = r.returncode
            except subprocess.TimeoutExpired:
                code = 'timeout (killed)'
    finally:
        release_place(place)
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
