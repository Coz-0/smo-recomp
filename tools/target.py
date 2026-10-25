"""Where the build's files are (used by the tools, scripts and tests).

The game is Super Mario Odyssey 1.4.1: the base game with its 1.4.1 update applied. Everything derived from your dump
goes under build/:
    build/dump/update   the program's modules (ExeFS, ELF conversions) and its control data
    build/<the rest>    romfs, manual, functions, lift, obj, gen, bin, run, ... laid out so that the runtime's paths
                        relative to its exe (..\\romfs, ..\\manual, ..\\userdata) work
"""
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VERSION = '1.4.1'                                       # the game version this project builds
BUILD = os.path.join(REPO, 'build')
DUMP = os.path.join(BUILD, 'dump', 'update')
RUNTIME = os.path.join(REPO, 'runtime')
# the host runtime's subsystem folders (runtime/<folder>); every one is an include directory of the runtime's code
RUNTIME_DIRS = ['core', 'libc', 'os', 'fs', 'gfx', 'audio', 'input', 'services', 'game']
# Where the guest's modules are mapped (runtime/core/hostrt.h has the same numbers; image.h carries them so the runtime
# checks they agree). On Windows at the console's own addresses; on macOS 0x10_0000_0000 higher, because macOS keeps
# 0x70_0000_0000..0x80_0000_0000 for malloc's heap range (docs/macos-port.md).
LAYOUT_SHIFT = 0x1000000000 if sys.platform == 'darwin' else 0
GUEST_BASE = 0x7100000000 + LAYOUT_SHIFT
SDK_BASE = 0x7500000000 + LAYOUT_SHIFT


def runtime(folder, name):
    """A file of the host runtime, e.g. runtime('gfx', 'mx_translate.c')."""
    return os.path.join(RUNTIME, folder, name)


def runtime_includes():
    """clang -I arguments for every runtime folder (the runtime's files include each other by bare name)."""
    return [arg for d in RUNTIME_DIRS for arg in ('-I', os.path.join(RUNTIME, d))]


def build(*parts):
    """A path in the build folder."""
    return os.path.join(BUILD, *parts)


def elf(module):
    """The converted module (main, sdk, subsdk0, rtld) of the program."""
    return os.path.join(DUMP, 'elf', module + '.elf')


def dump_version():
    """The display version of the extracted program (from its control data), or None."""
    p = os.path.join(DUMP, 'control.nacp')
    if not os.path.isfile(p):
        return None
    return open(p, 'rb').read()[0x3060:0x3070].split(b'\0')[0].decode('ascii', 'replace')
