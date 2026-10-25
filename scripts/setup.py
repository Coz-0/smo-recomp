#!/usr/bin/env python3
"""One entry point: from a fresh clone and your own dump to a runnable native build of the game.

    python scripts/setup.py --dump <folder> [--jobs N]

The game is Super Mario Odyssey 1.4.1. <folder> is your decrypted dump of it (docs/building.md: what to dump with
nxdumptool and how to lay it out): the program's ExeFS with the update, the RomFS with the update applied and the
update's control.nacp; the Help pages and the console's shared fonts are optional. No keys and no
firmware are read: the files are already decrypted by your console. Where the files go: tools/target.py.

Nothing of the game is stored in the repository: every step derives its files from the dump into <repo>/build (ignored
by git). Steps run in this order and are resumable: a step whose output exists is skipped (--redo <step> repeats it,
--from <step> repeats it and everything after it); compile, tables and link always run, and only rebuild what changed.

    deps       check Python packages, clang, the Vulkan SDK, SDL3 and the WebView2 SDK
    import     your dump -> build/dump (modules as ELF, versions); RomFS and Help read in place  (tools/extract/extract.py)
    functions  every function of each module, with exact boundaries                            (tools/extract/functions.py)
    lift       AArch64 -> C for every function -> build/lift/main                              (tools/lifter/recomp.py)
    compile    the lifted C -> objects (clang -O1, incremental)                                (tools/lifter/build_main.py)
    tables     generated headers and tables                                     (tools/lifter/gen_tables.py, gen_sdktest.py)
    sdk        the system-library functions that are lifted too                                 (tools/lifter/sdklift.py)
    link       host runtime + game library -> build/bin/smo_native.exe                          (tools/lifter/build_exe.py)
    check      differential test against Unicorn and link check (only with --only check or --check; minutes)

Other commands:  --status (what is done)   --steps (list)   --only <step>
Tool locations: LLVM_ROOT (clang 9, default C:/llvm-9.0.0), VULKAN_HEADERS (default the Vulkan SDK's Include folder),
SDL3_ROOT (default deps/SDL3-*), WEBVIEW2_ROOT (default deps/webview2-*). Then start the game with scripts/run.py.
"""
import argparse
import glob
import importlib
import os
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, 'tools'))
import target as T  # noqa: E402

BUILD = T.BUILD
PY = sys.executable


def tool(*parts):
    return os.path.join(REPO, 'tools', *parts)


def exists(*parts):
    return os.path.exists(os.path.join(BUILD, *parts))


def check_deps(a):
    ok = True
    for mod, pip in (('lz4', 'lz4'), ('capstone', 'capstone'), ('unicorn', 'unicorn'), ('numpy', 'numpy'), ('PIL', 'Pillow')):
        try:
            importlib.import_module(mod)
        except ImportError:
            print('  missing Python package %s (pip install %s)' % (mod, pip))
            ok = False
    vkh = T.vulkan_headers()
    vkh = os.path.join(vkh, 'vulkan', 'vulkan.h') if vkh else ''
    if T.MAC:                                         # docs/macos-port.md: Apple clang, Homebrew's SDL3, the LunarG SDK
        import shutil
        clang = shutil.which(T.clang()) or ''
        tools = (('Apple clang (xcode-select --install)', clang), ('Vulkan SDK (~/VulkanSDK or VULKAN_SDK)', vkh),
                 ('SDL3 (brew install sdl3, or SDL3_ROOT)', os.path.join(T.sdl3()[0], 'SDL3', 'SDL.h')))
    else:
        clang = T.clang()
        sdl = os.environ.get('SDL3_ROOT') or next(iter(sorted(glob.glob(os.path.join(REPO, 'deps', 'SDL3-*')))), '')
        wv2 = os.environ.get('WEBVIEW2_ROOT') or next(iter(sorted(glob.glob(os.path.join(REPO, 'deps', 'webview2-*')))[-1:]), '')
        wv2 = os.path.join(wv2, 'build', 'native', 'include', 'WebView2.h') if wv2 else ''
        tools = (('clang 9 (LLVM_ROOT)', clang), ('Vulkan SDK headers (VULKAN_HEADERS)', vkh), ('SDL3 (SDL3_ROOT or deps/SDL3-*)', sdl),
                 ('WebView2 SDK (WEBVIEW2_ROOT)', wv2))
    for what, path in tools:
        found = bool(path) and os.path.exists(path)
        print('  %-38s %s' % (what, path if found else 'NOT FOUND'))
        ok &= found
    if a.dump and not os.path.isdir(a.dump):
        print('  dump folder %s does not exist' % a.dump)
        ok = False
    return ok


def need_dump(a):
    if not a.dump:
        raise SystemExit('this step reads your dump: give --dump <folder of your decrypted dump> (docs/building.md)')
    return ['--dump', a.dump]


# name, what it makes (for --status; in the build folder), done test (None: always runs), commands
STEPS = [
    ('deps', 'tools found', None, None),
    ('import', 'dump, romfs', lambda: os.path.exists(T.elf('main')) and exists('romfs'),
     lambda a: [[PY, tool('extract', 'extract.py')] + need_dump(a)]),
    ('functions', 'functions', lambda: exists('functions', 'main_functions.tsv'),
     lambda a: [[PY, tool('extract', 'functions.py')]]),
    ('lift', 'lift/main', lambda: exists('lift', 'main', 'table.c'),
     lambda a: [[PY, tool('lifter', 'recomp.py'), 'full']]),
    ('compile', 'obj/main', None, lambda a: [[PY, tool('lifter', 'build_main.py'), str(a.jobs)]]),
    ('tables', 'gen', None,
     lambda a: [[PY, tool('lifter', 'gen_tables.py')], [PY, tool('lifter', 'gen_sdktest.py')]]),
    ('sdk', 'gen/sdklift_gen.c', lambda: exists('gen', 'sdklift_report.txt'),
     lambda a: [[PY, tool('lifter', 'sdklift.py'), 'gen']]),
    ('link', 'bin/' + T.EXE, None, lambda a: [[PY, tool('lifter', 'build_exe.py')]]),
    ('check', 'difftest + linkcheck', None,
     lambda a: [[PY, tool('lifter', 'difftest.py'), 'all'], [PY, tool('lifter', 'linkcheck.py')]]),
]
NAMES = [s[0] for s in STEPS]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--dump', help='folder of your decrypted dump (docs/building.md)')
    ap.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 2) - 2))
    ap.add_argument('--status', action='store_true')
    ap.add_argument('--steps', action='store_true')
    ap.add_argument('--only', choices=NAMES)
    ap.add_argument('--redo', choices=NAMES)
    ap.add_argument('--from', dest='from_step', choices=NAMES)
    ap.add_argument('--check', action='store_true', help='also run the check step at the end')
    a = ap.parse_args()

    if a.steps:
        print('\n'.join(NAMES))
        return
    if a.status:
        print('Super Mario Odyssey %s: %s' % (T.VERSION, os.path.relpath(BUILD, REPO)))
        for name, made, done, _ in STEPS:
            state = 'runs every time (incremental)' if done is None else ('done' if done() else 'to do')
            print('  %-10s %-28s %s' % (name, made, state if name not in ('deps', 'check') else 'on demand'))
        return

    if a.only:
        todo = [a.only]
    else:
        todo = [n for n in NAMES if n != 'check' or a.check]
    first = NAMES.index(a.from_step) if a.from_step else None
    for name, made, done, cmds in STEPS:
        if name not in todo:
            continue
        forced = a.only == name or a.redo == name or (first is not None and NAMES.index(name) >= first)
        if name == 'deps':
            print('[deps]')
            if not check_deps(a):
                raise SystemExit('some tools are missing (see docs/building.md)')
            continue
        if done is not None and done() and not forced:
            print('[%s] done (%s)' % (name, made))
        else:
            for cmd in cmds(a):
                print('[%s] %s' % (name, ' '.join(os.path.relpath(c, REPO) if c.startswith(REPO) else c for c in cmd[1:])), flush=True)
                r = subprocess.run(cmd, cwd=REPO)
                if r.returncode != 0:
                    raise SystemExit('[%s] failed (exit code %d); fix the cause and run setup again, it resumes here' % (name, r.returncode))
    if 'link' in todo:
        print('\nBuilt: %s. Start the game with: python scripts/run.py <run name> --seconds 300'
              % os.path.relpath(T.build('bin', T.EXE), REPO))


if __name__ == '__main__':
    main()
