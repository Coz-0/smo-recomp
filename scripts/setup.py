#!/usr/bin/env python3
"""One entry point: from a fresh clone and your own dump to a runnable native build of the game.

    python scripts/setup.py --dump <folder> [--keys <prod.keys>] [--fw <firmware folder>] [--jobs N]

<folder> holds what you dumped from YOUR console (see BUILDING.md): the base package of Super Mario Odyssey 1.0.0,
an .nsp with [0100000000010000] in its name (an update package [0100000000010800] next to it is read for its version
only). Keys come from --keys, %SMO_KEYS%, ~/.switch/prod.keys or Ryujinx's folder (tools/extract/switchfs.py). The
system fonts come from your firmware's .nca files (--fw; default: Ryujinx's installed firmware).

Nothing of the game is stored in the repository: every step derives its files from the dump into <repo>/build (ignored
by git). Steps run in this order and are resumable: a step whose output exists is skipped (--redo <step> repeats it,
--from <step> repeats it and everything after it); compile, tables and link always run, and only rebuild what changed.

    deps       check Python packages, clang, the Vulkan SDK and SDL3
    extract    dump -> ExeFS, main.elf / sdk.elf, NACP, version.json        (tools/extract/extract.py)
    romfs      the game's RomFS -> build/romfs                              (tools/extract/romfs.py)
    manual     the offline HTML document (the game's Help) -> build/manual  (tools/extract/romfs.py --manual)
    fonts      the system's shared fonts from your firmware                 (tools/extract/fw_fonts.py)
    functions  every function of main, with exact boundaries                (tools/extract/functions.py)
    lift       AArch64 -> C for every function -> build/lift/main          (tools/lifter/recomp.py)
    compile    the lifted C -> objects (clang -O1, incremental)            (tools/lifter/build_main.py)
    tables     generated headers and tables                                 (tools/lifter/gen_tables.py, gen_sdktest.py)
    sdk        the system-library functions that are lifted too             (tools/lifter/sdklift.py)
    link       host runtime + game library -> build/bin/smo_native.exe      (tools/lifter/build_exe.py)
    check      differential test against Unicorn and link check (only with --only check or --check; minutes)

Other commands:  --status (what is done)   --steps (list)   --only <step>
Tool locations: LLVM_ROOT (clang 9, default C:/llvm-9.0.0), VULKAN_HEADERS (default the Vulkan SDK's Include folder),
SDL3_ROOT (default deps/SDL3-*). Then start the game with scripts/run.py.
"""
import argparse
import glob
import importlib
import os
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(REPO, 'build')
PY = sys.executable


def tool(*parts):
    return os.path.join(REPO, 'tools', *parts)


def exists(*parts):
    return os.path.exists(os.path.join(BUILD, *parts))


def nonempty(*parts):
    p = os.path.join(BUILD, *parts)
    return os.path.isdir(p) and any(os.scandir(p))


def check_deps(a):
    ok = True
    for mod, pip in (('Crypto', 'pycryptodome'), ('lz4', 'lz4'), ('zstandard', 'zstandard'), ('capstone', 'capstone'),
                     ('unicorn', 'unicorn'), ('numpy', 'numpy'), ('PIL', 'Pillow'), ('yaml', 'pyyaml')):
        try:
            importlib.import_module(mod)
        except ImportError:
            print('  missing Python package %s (pip install %s)' % (mod, pip))
            ok = False
    clang = os.path.join(os.environ.get('LLVM_ROOT', 'C:/llvm-9.0.0'), 'bin', 'clang.exe')
    vkh = os.environ.get('VULKAN_HEADERS') or next(iter(sorted(glob.glob('C:/VulkanSDK/*/Include'))), '')
    sdl = os.environ.get('SDL3_ROOT') or next(iter(sorted(glob.glob(os.path.join(REPO, 'deps', 'SDL3-*')))), '')
    for what, path in (('clang 9 (LLVM_ROOT)', clang), ('Vulkan SDK headers (VULKAN_HEADERS)', vkh), ('SDL3 (SDL3_ROOT or deps/SDL3-*)', sdl)):
        found = bool(path) and os.path.exists(path)
        print('  %-38s %s' % (what, path if found else 'NOT FOUND'))
        ok &= found
    if a.dump and not os.path.isdir(a.dump):
        print('  dump folder %s does not exist' % a.dump)
        ok = False
    return ok


def need_dump(a):
    if not a.dump:
        raise SystemExit('this step reads your dump: give --dump <folder with your base NSP>')
    return ['--dir', a.dump]


def keys(a):
    return ['--keys', a.keys] if a.keys else []


def decomp(a):
    return ['--decomp', a.decomp] if a.decomp else []


# name, what it makes (for --status), done test (None: always runs), commands
STEPS = [
    ('deps', 'tools found', None, None),
    ('extract', 'build/dump', lambda: exists('dump', 'base', 'elf', 'main.elf'),
     lambda a: [[PY, tool('extract', 'extract.py')] + need_dump(a) + keys(a) + decomp(a)]),
    ('romfs', 'build/romfs', lambda: nonempty('romfs'),
     lambda a: [[PY, tool('extract', 'romfs.py')] + need_dump(a) + keys(a)]),
    ('manual', 'build/manual', lambda: nonempty('manual'),
     lambda a: [[PY, tool('extract', 'romfs.py'), '--manual'] + need_dump(a) + keys(a)]),
    ('fonts', 'build/sysdata/fonts', lambda: nonempty('sysdata', 'fonts'),
     lambda a: [[PY, tool('extract', 'fw_fonts.py')] + (['--fw', a.fw] if a.fw else []) + keys(a)]),
    ('functions', 'build/functions', lambda: exists('functions', 'main_functions.tsv'),
     lambda a: [[PY, tool('extract', 'functions.py')] + decomp(a)]),
    ('lift', 'build/lift/main', lambda: exists('lift', 'main', 'table.c'),
     lambda a: [[PY, tool('lifter', 'recomp.py'), 'full']]),
    ('compile', 'build/obj/main', None, lambda a: [[PY, tool('lifter', 'build_main.py'), str(a.jobs)]]),
    ('tables', 'build/gen', None,
     lambda a: [[PY, tool('lifter', 'gen_tables.py')], [PY, tool('lifter', 'gen_sdktest.py')]]),
    ('sdk', 'build/gen/sdklift_gen.c', lambda: exists('gen', 'sdklift_report.txt'),
     lambda a: [[PY, tool('lifter', 'sdklift.py'), 'gen']]),
    ('link', 'build/bin/smo_native.exe', None, lambda a: [[PY, tool('lifter', 'build_exe.py')]]),
    ('check', 'difftest + linkcheck', None,
     lambda a: [[PY, tool('lifter', 'difftest.py'), 'all'], [PY, tool('lifter', 'linkcheck.py')]]),
]
NAMES = [s[0] for s in STEPS]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--dump', help='folder holding your base NSP (and optionally the update NSP)')
    ap.add_argument('--keys', help='your prod.keys (default: searched, see tools/extract/switchfs.py)')
    ap.add_argument('--fw', help="folder of your firmware's .nca files, for the system fonts")
    ap.add_argument('--decomp', help='a local OdysseyDecomp clone (optional: names in the function list, a version check)')
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
                raise SystemExit('some tools are missing (see BUILDING.md)')
            continue
        if done is not None and done() and not forced:
            print('[%s] done (%s)' % (name, made))
            continue
        for cmd in cmds(a):
            print('[%s] %s' % (name, ' '.join(os.path.relpath(c, REPO) if c.startswith(REPO) else c for c in cmd[1:])), flush=True)
            r = subprocess.run(cmd, cwd=REPO)
            if r.returncode != 0:
                raise SystemExit('[%s] failed (exit code %d); fix the cause and run setup again, it resumes here' % (name, r.returncode))
    if 'link' in todo:
        print('\nBuilt: build/bin/smo_native.exe. Start the game with: python scripts/run.py <run name> --seconds 300')


if __name__ == '__main__':
    main()
