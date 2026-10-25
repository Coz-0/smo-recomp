"""Where the build's files are (used by the tools, scripts and tests).

The game is Super Mario Odyssey 1.3.0: the base game with its 1.3.0 update applied. Everything derived from your dump
goes under build/:
    build/dump/update   the program's modules (ExeFS, ELF conversions) and its control data
    build/<the rest>    romfs, manual, functions, lift, obj, gen, bin, run, ... laid out so that the runtime's paths
                        relative to its exe (..\\romfs, ..\\manual, ..\\userdata) work
"""
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VERSION = '1.3.0'                                       # the game version this project builds
BUILD = os.environ.get('SMO_BUILD_DIR') or os.path.join(REPO, 'build')     # SMO_BUILD_DIR: a build folder elsewhere (tests)
DUMP = os.path.join(BUILD, 'dump', 'update')
RUNTIME = os.path.join(REPO, 'runtime')
# the host runtime's subsystem folders (runtime/<folder>); every one is an include directory of the runtime's code
RUNTIME_DIRS = ['core', 'libc', 'os', 'fs', 'gfx', 'audio', 'input', 'services', 'game']
# Where the guest's modules are mapped (runtime/core/hostrt.h has the same numbers; image.h carries them so the runtime
# checks they agree). On Windows at the console's own addresses; on macOS 0x10_0000_0000 higher, because macOS keeps
# 0x70_0000_0000..0x80_0000_0000 for malloc's heap range (docs/macos-port.md).
MAC = sys.platform == 'darwin'
LAYOUT_SHIFT = 0x1000000000 if MAC else 0
GUEST_BASE = 0x7100000000 + LAYOUT_SHIFT
SDK_BASE = 0x7500000000 + LAYOUT_SHIFT


def runtime(folder, name):
    """A file of the host runtime, e.g. runtime('gfx', 'mx_translate.c')."""
    return os.path.join(RUNTIME, folder, name)


def runtime_includes():
    """clang -I arguments for every runtime folder (the runtime's files include each other by bare name); on macOS also
    runtime/compat, the Win32 calls of the runtime on POSIX and Mach (docs/macos-port.md)."""
    dirs = RUNTIME_DIRS + (['compat'] if MAC else [])
    return [arg for d in dirs for arg in ('-I', os.path.join(RUNTIME, d))]


# ---- the toolchain and file names of this system (Windows: clang 9 and the MSVC linker; macOS: Apple clang)
OBJ_EXT = '.o' if MAC else '.obj'
EXE = 'smo_native' if MAC else 'smo_native.exe'          # the program a person plays (scripts/run.py copies it)
GAME_LIB = 'libsmo_game.dylib' if MAC else 'smo_game.dll'  # the lifted game (build_exe.py)


def clang():
    """The C compiler: LLVM_ROOT's clang when set; else clang 9 in C:/llvm-9.0.0 on Windows, Apple clang on macOS."""
    root = os.environ.get('LLVM_ROOT')
    if MAC:
        return os.path.join(root, 'bin', 'clang') if root else 'clang'
    return os.path.join(root or 'C:/llvm-9.0.0', 'bin', 'clang.exe')


def vulkan_sdk():
    """The Vulkan SDK's folder for this system (VULKAN_SDK when set; macOS: its macOS folder), or ''."""
    import glob
    if os.environ.get('VULKAN_SDK'):
        return os.environ['VULKAN_SDK']
    if MAC:
        return next(iter(sorted(glob.glob(os.path.expanduser('~/VulkanSDK/*/macOS')))[-1:]), '')
    return next(iter(sorted(glob.glob('C:/VulkanSDK/*'))[-1:]), '')


def vulkan_headers():
    """The folder holding vulkan/vulkan.h (VULKAN_HEADERS wins), or ''."""
    if os.environ.get('VULKAN_HEADERS'):
        return os.environ['VULKAN_HEADERS']
    sdk = vulkan_sdk()
    return os.path.join(sdk, 'include' if MAC else 'Include') if sdk else ''


def sdl3():
    """SDL3 3.4.x: SDL3_ROOT, else deps/SDL3-3.4.18 on Windows and Homebrew's on macOS: (include folder, library folder)."""
    root = os.environ.get('SDL3_ROOT')
    if MAC:
        root = root or '/opt/homebrew/opt/sdl3'
        return os.path.join(root, 'include'), os.path.join(root, 'lib')
    root = root or os.path.join(REPO, 'deps', 'SDL3-3.4.18')
    return os.path.join(root, 'include'), os.path.join(root, 'lib', 'x64')


# what the host runtime links against on macOS (its Win32 calls come from runtime/compat/win32_posix.c)
MAC_FRAMEWORKS = ['CoreFoundation', 'CoreGraphics', 'CoreText', 'AudioToolbox', 'IOKit', 'Foundation', 'AVFoundation', 'CoreMedia',
                  'CoreVideo', 'AppKit', 'WebKit']


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
