"""Build build/bin/mx_equiv.exe (tests/mx_equiv.c + runtime/gfx/mx_translate.c) with clang 9 and the Vulkan SDK.

    python tests/build_mx_equiv.py
    build\\bin\\mx_equiv.exe <folder of vk_prog_*_1.bin> [max]      (run from build\\bin: it loads smo_glslang.dll)
"""
import glob
import os
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, 'tools'))
import target as T  # noqa: E402
CLANG = T.clang()
if T.POSIX:                                       # macOS, Linux: the system's (or the SDK's) loader, the Win32 shim
    VKH = T.vulkan_headers()
    VKLIB = '-lvulkan'
    extra = [T.runtime('compat', 'win32_posix.c'), '-lpthread', '-ldl', '-lm'] + (T.LINUX_CFLAGS if T.LINUX else [])
    out = T.build('bin', 'mx_equiv')
else:
    VKH = os.environ.get('VULKAN_HEADERS') or next(iter(sorted(glob.glob('C:/VulkanSDK/*/Include'))), '')
    VKLIB = os.path.join(os.path.dirname(VKH), 'Lib', 'vulkan-1.lib')
    extra = []
    out = T.build('bin', 'mx_equiv.exe')
cmd = [CLANG, '-O1', '-w'] + T.runtime_includes() + ['-I', VKH,
       os.path.join(REPO, 'tests', 'mx_equiv.c'), T.runtime('gfx', 'mx_translate.c'), VKLIB] + extra + ['-o', out]
r = subprocess.run(cmd, capture_output=True, text=True)
print((r.stdout + r.stderr)[-4000:])
print('built %s' % out if r.returncode == 0 else 'FAILED')
sys.exit(r.returncode)
