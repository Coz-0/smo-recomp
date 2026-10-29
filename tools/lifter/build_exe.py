"""Link the runnable program: build/bin/smo_game.dll (lifted main + game_shim.c) and build/bin/smo_native.exe (host
runtime + gamedll.c); adapted from ssbu-recomp's build.py `link` step (GPL-3.0-or-later).

    python tools/lifter/build_exe.py [--exe smo_native.exe] [--force]

Needs: build/obj/main/*.obj (build_main.py), build/gen/* (gen_tables.py, gen_sdktest.py), SDL3 (deps/SDL3-3.4.18 or
SDL3_ROOT), the WebView2 SDK (deps/webview2-<version>, from the Microsoft.Web.WebView2 NuGet package, or WEBVIEW2_ROOT:
its header and WebView2Loader.dll, for the Help page in the game window), Vulkan headers (VULKAN_HEADERS, default the
Vulkan SDK's Include folder), clang 9 (LLVM_ROOT).
Host files are recompiled when they or any runtime header / generated file changed. Prints every compiler error.

Linux (docs/linux-port.md): build/bin/libsmo_game.so and build/bin/smo_native, with the system's clang, SDL3, Vulkan
headers, shaderc, FreeType, FFmpeg and OpenSSL; no WebView2 (Help opens in the web browser).

macOS (docs/macos-port.md): build/bin/libsmo_game.dylib and build/bin/smo_native, with Apple clang; SDL3 from Homebrew
(or SDL3_ROOT), the Vulkan SDK from ~/VulkanSDK (or VULKAN_SDK); no WebView2. The program finds the game library next to
itself (@executable_path).
"""
import concurrent.futures
import glob
import os
import shutil
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, 'tools'))
import target as T  # noqa: E402
GEN = T.build('gen')
LIFT = T.build('lift', 'main')
OBJ_GAME = T.build('obj', 'main')
BIN = T.build('bin')
CLANG = T.clang()
SDL3_INC, SDL3_LIB = T.sdl3()
WEBVIEW2 = os.environ.get('WEBVIEW2_ROOT') or next(iter(sorted(glob.glob(os.path.join(REPO, 'deps', 'webview2-*')))[-1:]), '')
VKH = T.vulkan_headers()

# The host runtime's build units by subsystem folder (runtime/<folder>/<file>). Not listed: nvn_vk_draw.c,
# nvn_vk_pipelines.c, nvn_vk_vlayout.c and nvn_vk_watchdog.c (included by nvn_vk.c), game_shim.c (part of the game DLL) and
# vk_shaderc_shim.c (smo_glslang.dll).
HOST_SOURCES = {
    'core': ['hostrt.c', 'boot.c', 'gamedll.c', 'sdklift_rt.c', 'hitch.c'],
    'libc': ['clib.c', 'nnlibc2.c', 'libcxx.c', 'nncxxlib.c', 'nncxxrt.c', 'nniostream.c', 'nnunwind.c'],
    'os': ['os_threads.c', 'nnsdk.c', 'nntime.c', 'nnmisc.c', 'smo_nn.c'],
    'fs': ['nnfs.c', 'nnfsdir.c'],
    'gfx': ['nvn_stub.c', 'nvn_gl.c', 'nvn_vk.c', 'nvn_misc.c', 'nvn_handles.c', 'nvn_layout.c', 'mx_translate.c',
            'tex_decode.c', 'display.c', 'gfx_options.c'],
    'audio': ['nnaudio.c', 'smo_audio.c'],
    'input': ['nnhid.c', 'nnpad_sdl.c'],
    'services': ['nnservices.c', 'nnapplet.c', 'nnalbum.c', 'nnnet.c', 'nnpl.c', 'nnfontll.c', 'nnmovie.c', 'nnmovie_mf.c'],
    'game': ['smo_nerve.c', 'smo_fast.c', 'smo_selftest.c', 'smo_ui.c', 'smo_amiibo.c', 'smo_guide.c'],
}
INCLUDED = [T.runtime('gfx', n) for n in ('nvn_vk_draw.c', 'nvn_vk_pipelines.c', 'nvn_vk_vlayout.c', 'nvn_vk_watchdog.c')]
GEN_SOURCES = ['sdkdata.c', 'sdklift_gen.c']
# RECOMP_HOST_OPT (default -O2) and RECOMP_ARCH (as build_main.py: e.g. -march=znver2) for the runtime; a change of them
# recompiles nothing by itself: delete build\obj\host (or touch a runtime header) first
HOST_FLAGS = [os.environ.get('RECOMP_HOST_OPT', '-O2'), '-w', '-fno-strict-aliasing'] + os.environ.get('RECOMP_ARCH', '').split() + T.runtime_includes() + ['-I', GEN, '-I', LIFT,
              '-Dfn_table=(*game_fn_table_p)', '-Dfn_table_count=(*game_fn_table_count_p)',
              '-Ddyn_syms=(*game_dyn_syms_p)', '-Ddyn_sym_count=(*game_dyn_sym_count_p)',
              '-I', SDL3_INC, '-I', VKH] + ([] if T.POSIX else ['-I', os.path.join(WEBVIEW2, 'build', 'native', 'include')])
LIBS = ['-luser32', '-lgdi32', '-lwinmm', '-lmfplat', '-lmfreadwrite', '-lmfuuid', '-lole32', '-lshlwapi', '-lshell32']
if T.MAC:
    HOST_SOURCES['compat'] = ['win32_posix.c', 'gl_stub.c']
    HOST_SOURCES['os'] = HOST_SOURCES['os'] + ['core_model.c']
    HOST_SOURCES['services'] = HOST_SOURCES['services'] + ['nnmovie_av.m']      # nnmovie_mf.c's reader on AVFoundation
    HOST_SOURCES['game'] = HOST_SOURCES['game'] + ['smo_guide_mac.m']           # the Help in the game window (WKWebView)
    LIBS = ['-L', SDL3_LIB, '-lSDL3', '-Wl,-rpath,' + SDL3_LIB] + [a for f in T.MAC_FRAMEWORKS for a in ('-framework', f)]
if T.LINUX:
    HOST_SOURCES['compat'] = ['win32_posix.c', 'gl_stub.c']
    HOST_SOURCES['os'] = HOST_SOURCES['os'] + ['core_model.c']
    HOST_SOURCES['services'] = HOST_SOURCES['services'] + ['nnmovie_ff.c']      # nnmovie_mf.c's reader on FFmpeg
    HOST_FLAGS += T.LINUX_CFLAGS
    LIBS = T.LINUX_LIBS


def fresh_output(path):
    """macOS: a program or library is removed before it is linked again. Written over in place, the kernel keeps the old
    code signature of the file and kills its next launch (SIGKILL; seen with smo_native)."""
    if T.MAC and os.path.exists(path):
        os.remove(path)


def build_game_dll(force):
    dll = os.path.join(BIN, T.GAME_LIB)
    lib = dll if T.POSIX else dll[:-4] + '.lib'         # macOS, Linux: the program links the library itself
    shim = T.runtime('core', 'game_shim.c')
    objs = sorted(glob.glob(os.path.join(OBJ_GAME, '*' + T.OBJ_EXT)))
    if not objs:
        raise SystemExit('no objects in %s: run tools/lifter/build_main.py first' % OBJ_GAME)
    newest = max([os.path.getmtime(o) for o in objs] + [os.path.getmtime(shim)])
    if not force and os.path.exists(dll) and os.path.exists(lib) and os.path.getmtime(dll) >= newest:
        print('game DLL up to date: %s' % dll)
        return 0
    rsp = os.path.join(BIN, 'smo_game.rsp')
    with open(rsp, 'w') as f:
        f.write('\n'.join('"%s"' % o.replace('\\', '/') for o in objs))
    t0 = time.time()
    mac = ['-Wl,-install_name,@rpath/' + T.GAME_LIB] if T.MAC else []       # found next to the program (@executable_path)
    if T.LINUX:     # calls inside the library bind to it at link time (no interposition), as they do in a DLL
        mac = ['-fPIC', '-fuse-ld=lld', '-Wl,--no-dependent-libraries', '-Wl,-Bsymbolic', '-Wl,-soname,' + T.GAME_LIB] + T.LINUX_CFLAGS
    fresh_output(dll)
    r = subprocess.run([CLANG, '-dynamiclib' if T.MAC else '-shared', '-O1', '-w'] + T.runtime_includes() + ['-I', LIFT] + mac
                       + [shim, '@' + rsp, '-o', dll], capture_output=True, text=True)
    out = (r.stdout + r.stderr).strip()
    if r.returncode:
        print(out[-4000:])
        print('could not link %s (is a game exe still running and holding it open?)' % dll)
        return 1
    print('linked %s: %.0f MB in %.0fs' % (dll, os.path.getsize(dll) / 2 ** 20, time.time() - t0))
    return 0


def compile_host(job):
    src, obj = job
    arc = ['-fobjc-arc'] if src.endswith('.m') else []          # Objective-C (macOS): ARC, as nnmovie_av.m requires
    r = subprocess.run([CLANG, '-c'] + HOST_FLAGS + arc + [src, '-o', obj], capture_output=True, text=True)
    return src, r.returncode, (r.stdout + r.stderr)


def main():
    force = '--force' in sys.argv
    exe = sys.argv[sys.argv.index('--exe') + 1] if '--exe' in sys.argv else T.EXE
    os.makedirs(BIN, exist_ok=True)
    if build_game_dll(force):
        return 1
    objdir = T.build('obj', 'host', os.path.splitext(exe)[0])
    os.makedirs(objdir, exist_ok=True)
    deps = (glob.glob(os.path.join(T.RUNTIME, '**', '*.h'), recursive=True) +
            glob.glob(os.path.join(T.RUNTIME, '**', '*.inc'), recursive=True) +
            glob.glob(os.path.join(GEN, '*.h')) + INCLUDED + [os.path.abspath(__file__)])
    newest_dep = max(os.path.getmtime(p) for p in deps)
    objs, jobs = [], []
    sources = [T.runtime(d, n) for d, names in HOST_SOURCES.items() for n in names]
    for src in sources + [os.path.join(GEN, n) for n in GEN_SOURCES]:
        obj = os.path.join(objdir, os.path.basename(src)[:-2] + T.OBJ_EXT)
        objs.append(obj)
        if force or not os.path.exists(obj) or os.path.getmtime(obj) < max(os.path.getmtime(src), newest_dep):
            jobs.append((src, obj))
    t0 = time.time()
    failed = 0
    with concurrent.futures.ThreadPoolExecutor(16) as pool:
        for src, code, err in pool.map(compile_host, jobs):
            if code:
                failed += 1
                print('FAILED %s\n%s' % (os.path.basename(src), err[-6000:]))
    print('host files: %d compiled, %d failed (%.0fs)' % (len(jobs), failed, time.time() - t0))
    if failed:
        return 1
    out = os.path.join(BIN, exe)
    if T.MAC:
        cmd = [CLANG] + objs + [os.path.join(BIN, T.GAME_LIB), '-Wl,-rpath,@executable_path'] + LIBS \
            + ['-Wl,-map,' + os.path.splitext(out)[0] + '.map', '-o', out]
    elif T.LINUX:   # -rdynamic: the game library and the shim's GetProcAddress find the program's own functions
        cmd = [CLANG, '-fuse-ld=lld', '-rdynamic', '-Wl,--no-dependent-libraries'] + objs + [os.path.join(BIN, T.GAME_LIB), "-Wl,-rpath,$ORIGIN"] + LIBS \
            + ['-Wl,-Map,' + out + '.map', '-o', out]
    else:
        cmd = [CLANG] + objs + [os.path.join(BIN, 'smo_game.lib'), os.path.join(SDL3_LIB, 'SDL3.lib')] \
            + LIBS + ['-Xlinker', '/MAP:' + out[:-4] + '.map', '-o', out]
    fresh_output(out)
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        print((r.stdout + r.stderr)[-8000:])
        return 1
    if not T.POSIX:
        copy_if_changed(os.path.join(SDL3_LIB, 'SDL3.dll'), os.path.join(BIN, 'SDL3.dll'))
        copy_if_changed(os.path.join(WEBVIEW2, 'build', 'native', 'x64', 'WebView2Loader.dll'), os.path.join(BIN, 'WebView2Loader.dll'))
    print('linked %s (%.1f MB)' % (out, os.path.getsize(out) / 2 ** 20))
    return build_shader_shim()


def copy_if_changed(src, dst):
    """Copies a DLL next to the exe unless the same file is there already (a running game keeps it open)."""
    if os.path.exists(dst) and os.path.getsize(dst) == os.path.getsize(src) and os.path.getmtime(dst) >= os.path.getmtime(src):
        return
    shutil.copy2(src, dst)


def build_shader_shim():
    """smo_glslang.dll from runtime/gfx/vk_shaderc_shim.c (see its header for why not glslang's static libraries), plus the
    Vulkan SDK's shaderc_shared.dll beside it; then a self-test: one vertex shader must compile to SPIR-V."""
    import ctypes
    dll = os.path.join(BIN, T.SHADER_LIB)
    src = T.runtime('gfx', 'vk_shaderc_shim.c')
    if not os.path.exists(dll) or os.path.getmtime(dll) < os.path.getmtime(src):
        fresh_output(dll)
        mac = T.runtime_includes() + ['-Wl,-install_name,@rpath/libsmo_glslang.dylib', T.runtime('compat', 'win32_posix.c')] \
            + [a for f in T.MAC_FRAMEWORKS for a in ('-framework', f)] if T.MAC else \
            T.runtime_includes() + T.LINUX_CFLAGS + ['-fPIC', T.runtime('compat', 'win32_posix.c'), '-ldl', '-lpthread'] if T.LINUX else []
        r = subprocess.run([CLANG, '-dynamiclib' if T.MAC else '-shared', '-O1', '-w'] + mac + [src, '-o', dll], capture_output=True, text=True)
        if r.returncode:
            print((r.stdout + r.stderr)[-3000:])
            return 1
    if T.LINUX:
        pass                                            # the system's libshaderc_shared.so.1, found by the loader
    elif T.MAC:
        sdk_lib = os.path.join(T.vulkan_sdk(), 'lib', 'libshaderc_shared.1.dylib')
        if os.path.exists(sdk_lib):
            copy_if_changed(sdk_lib, os.path.join(BIN, 'libshaderc_shared.1.dylib'))
    else:
        sdk_bin = os.path.join(os.path.dirname(VKH), 'Bin', 'shaderc_shared.dll')
        if os.path.exists(sdk_bin):
            copy_if_changed(sdk_bin, os.path.join(BIN, 'shaderc_shared.dll'))
    lib = ctypes.CDLL(dll)
    words, count, log = ctypes.POINTER(ctypes.c_uint32)(), ctypes.c_size_t(), ctypes.create_string_buffer(4096)
    ok = lib.smo_glsl_to_spirv(0, b'#version 460\nvoid main() { gl_Position = vec4(float(gl_VertexIndex), 0.0, 0.0, 1.0); }\n',
                               ctypes.byref(words), ctypes.byref(count), log, 4096)
    magic = words[0] if ok and count.value else 0
    print('shader shim: %s, test vertex shader -> %d SPIR-V words, magic 0x%08x %s%s'
          % (dll, count.value, magic, 'OK' if magic == 0x07230203 else 'FAILED', (' log: ' + log.value.decode()) if log.value else ''))
    if ok:
        lib.smo_spirv_free(words)
    return 0 if magic == 0x07230203 else 1


if __name__ == '__main__':
    sys.exit(main())
