"""Compile every shader stage of the game to SPIR-V ahead of play, into the renderer's cache (build/cache/vk/spv).

    python tools/lifter/precompile_shaders.py [--jobs N]

The renderer translates a stage when the game hands its program over and compiles the text to SPIR-V; it keeps the
SPIR-V as build/cache/vk/spv/<hash of the text>.spv and reads that file instead of compiling the next time
(nvn_vk_pipelines.c module_from_glsl). Compiling is nearly all of the cost (measured on Linux, Ryzen 5 7535HS, one
thread: translating all 27,888 stages 4.9 s, compiling them 285 s). This step writes those files before the first
start: tests/check_all_shaders.py --extract-only takes every stage out of the game's shader containers (build/shaders_all),
then tests/mx_all.c translates each with the renderer's bindings and compiles it, one process per processor. Files that
are there already are left alone, so a second run only does what is new.
A stage the renderer meets with another key (integer attributes or textures, forced outputs) or with an internal
resolution scale other than 1 (graphics.ini) gives another text: that one is still compiled in play.
"""
import argparse
import os
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, 'tools'))
import target as T  # noqa: E402

STAGES = T.build('shaders_all')
BIN = T.build('bin')
CACHE = os.environ.get('SMO_VK_CACHE') or T.build('cache', 'vk')


def build_mx_all():
    posix = getattr(T, 'POSIX', False)            # macOS, Linux (their ports); Windows otherwise
    exe = os.path.join(BIN, 'mx_all' if posix else 'mx_all.exe')
    srcs = [os.path.join(REPO, 'tests', 'mx_all.c'), T.runtime('gfx', 'mx_translate.c')]
    if os.path.exists(exe) and os.path.getmtime(exe) >= max(os.path.getmtime(s) for s in srcs):
        return exe
    extra = [T.runtime('compat', 'win32_posix.c'), '-lpthread', '-ldl', '-lm'] + getattr(T, 'LINUX_CFLAGS', []) if posix else []
    clang = T.clang() if hasattr(T, 'clang') else os.path.join(os.environ.get('LLVM_ROOT', 'C:/llvm-9.0.0'), 'bin', 'clang.exe')
    r = subprocess.run([clang, '-O1', '-w'] + T.runtime_includes() + srcs + extra + ['-o', exe], capture_output=True, text=True)
    if r.returncode:
        print((r.stdout + r.stderr)[-3000:])
        sys.exit('precompile_shaders: the build of %s failed' % exe)
    return exe


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    a = ap.parse_args()
    t0 = time.time()
    if not os.path.isdir(STAGES) or not os.listdir(STAGES):
        r = subprocess.run([sys.executable, os.path.join(REPO, 'tests', 'check_all_shaders.py'), '--extract-only'],
                           capture_output=True, text=True)
        print(r.stdout.strip().split('\n')[-1])
        if r.returncode:
            sys.exit('precompile_shaders: the stages could not all be taken out of the game\'s files')
    exe = build_mx_all()
    os.makedirs(os.path.join(CACHE, 'spv'), exist_ok=True)
    procs = [subprocess.Popen([exe, STAGES, '--spv', CACHE, str(k), str(a.jobs)], cwd=BIN, stdout=subprocess.PIPE, text=True)
             for k in range(a.jobs)]
    written = had = 0
    bad = []
    for p in procs:
        out, _ = p.communicate()
        for line in out.split('\n'):
            if line.startswith('SPIR-V files:'):
                w, h = (int(x.split()[0]) for x in line.split(':', 1)[1].split(','))
                written += w
                had += h
        if p.returncode:
            bad.append(out.strip().split('\n')[-3:])
    print('shaders: %d SPIR-V files written, %d already there, in %.0f s on %d processes -> %s'
          % (written, had, time.time() - t0, a.jobs, os.path.join(CACHE, 'spv')))
    for b in bad:
        print('  a stage was not translated or compiled: %s' % ' | '.join(b))
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
