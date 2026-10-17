"""Which game version a build is for, and where its files go (used by the tools, scripts and tests).

SMO_TARGET=1.0.0 (the default): the base package alone; its modules in build/dump/base, everything else under build/.
SMO_TARGET=<your update's version> (1.4.1): the update package applied; its modules in build/dump/update and a build
folder of its own, build/<version>/, laid out as build/ is (romfs, manual, functions, lift, obj, gen, bin, run, ...), so
the runtime's paths relative to its exe (..\\romfs, ..\\manual, ..\\userdata) work unchanged. build/dump is shared.
scripts/setup.py --target sets SMO_TARGET for every step it runs.
"""
import os

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASE_VERSION = '1.0.0'
TARGET = os.environ.get('SMO_TARGET') or BASE_VERSION
IS_BASE = TARGET == BASE_VERSION
BUILD = os.path.join(REPO, 'build') if IS_BASE else os.path.join(REPO, 'build', TARGET)
DUMP_KIND = 'base' if IS_BASE else 'update'          # also the key of the package in build/dump/version.json
DUMP = os.path.join(REPO, 'build', 'dump', DUMP_KIND)


def build(*parts):
    """A path in this target's build folder."""
    return os.path.join(BUILD, *parts)


def elf(module):
    """The converted module (main, sdk, subsdk0, rtld) of this target's package."""
    return os.path.join(DUMP, 'elf', module + '.elf')


def update_version():
    """The display version of the extracted update package (from its NACP), or None."""
    p = os.path.join(REPO, 'build', 'dump', 'update', 'control.nacp')
    if not os.path.isfile(p):
        return None
    return open(p, 'rb').read()[0x3060:0x3070].split(b'\0')[0].decode('ascii', 'replace')
