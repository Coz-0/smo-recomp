/* smo_glslang.dll - GLSL (Vulkan dialect) -> SPIR-V through the Vulkan SDK's shaderc_shared.dll (the exported
 * interface of ssbu-recomp's vk_glslang_shim.c, which linked glslang's static libraries instead).
 *
 * Why not the static libraries: the glslang static libraries of the Vulkan SDK are built with MSVC 2022
 * and need its vectorised STL helpers (__std_find_trivial_4, ...), which the installed VS 2019 Build Tools' runtime does
 * not have (link error LNK2019, measured). shaderc_shared.dll is the same glslang front end packaged as a self-contained
 * DLL with a C API, so nothing C++ crosses a module boundary. It is loaded at the first call (LoadLibrary), next to the
 * exe or from the Vulkan SDK's Bin folder (build_exe.py copies it beside the exe).
 *
 * Settings (those of ssbu-recomp's shim): Vulkan 1.3 environment, SPIR-V 1.6, no optimisation (shaderc's default
 * level is zero), entry point "main". Every source mx_translate.c / nvn_vk_draw.c produce starts with "#version 460".
 * stage: 0 vertex, 1 fragment, 5 compute (the MX_* numbers of mx_translate.h).
 * Returns 1 and a malloc'ed word array (free it with smo_spirv_free), or 0 with the compiler's messages in log.
 * Thread-safe: the compiler object is made once; shaderc compilers are safe to use from several threads. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include "win32_posix.h"
#include <dlfcn.h>
#endif

typedef void *shc_compiler;
typedef void *shc_options;
typedef void *shc_result;
enum { SHC_VERTEX = 0, SHC_FRAGMENT = 1, SHC_COMPUTE = 2, SHC_GEOMETRY = 3 };      /* shaderc_shader_kind */
enum { SHC_ENV_VULKAN = 0 };                                           /* shaderc_target_env_vulkan */
#define SHC_ENV_VULKAN_1_3 ((1u << 22) | (3 << 12))                    /* shaderc_env_version_vulkan_1_3 */
#define SHC_SPIRV_1_6 0x010600u                                        /* shaderc_spirv_version_1_6 */

static struct {
    shc_compiler (*compiler_initialize)(void);
    shc_options (*options_initialize)(void);
    void (*options_set_target_env)(shc_options, int, uint32_t);
    void (*options_set_target_spirv)(shc_options, uint32_t);
    shc_result (*compile_into_spv)(shc_compiler, const char *, size_t, int, const char *, const char *, shc_options);
    int (*result_get_compilation_status)(shc_result);
    size_t (*result_get_length)(shc_result);
    const char *(*result_get_bytes)(shc_result);
    const char *(*result_get_error_message)(shc_result);
    void (*result_release)(shc_result);
} S;
static shc_compiler g_compiler;
static shc_options g_options;
static char g_load_error[256];
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK init_once(PINIT_ONCE once, PVOID arg, PVOID *ctx) {
    (void)once; (void)arg; (void)ctx;
#ifdef _WIN32
    HMODULE m = LoadLibraryA("shaderc_shared.dll");
    if (!m) {                                                          /* the Vulkan SDK's own copy */
        char path[MAX_PATH];
        const char *sdk = getenv("VULKAN_SDK");
        if (sdk) { snprintf(path, sizeof path, "%s\\Bin\\shaderc_shared.dll", sdk); m = LoadLibraryA(path); }
    }
#else
    /* macOS: the copy build_exe.py puts next to this library, else the Vulkan SDK's (VULKAN_SDK = its macOS folder) */
    HMODULE m = NULL;
    {
        Dl_info di;
        char path[2048];
        if (dladdr((void *)init_once, &di) && di.dli_fname) {
            snprintf(path, sizeof path, "%s", di.dli_fname);
            char *sl = strrchr(path, '/');
            if (sl) { snprintf(sl + 1, sizeof path - (size_t)(sl + 1 - path), "libshaderc_shared.1.dylib"); m = LoadLibraryA(path); }
        }
        const char *sdk = getenv("VULKAN_SDK");
        if (!m && sdk) { snprintf(path, sizeof path, "%s/lib/libshaderc_shared.1.dylib", sdk); m = LoadLibraryA(path); }
    }
#endif
    if (!m) { snprintf(g_load_error, sizeof g_load_error, "shaderc_shared.dll cannot be loaded (Vulkan SDK Bin folder)"); return FALSE; }
#define GET(field, name) if (!(*(FARPROC *)&S.field = GetProcAddress(m, name))) { snprintf(g_load_error, sizeof g_load_error, "shaderc_shared.dll has no %s", name); return FALSE; }
    GET(compiler_initialize, "shaderc_compiler_initialize")
    GET(options_initialize, "shaderc_compile_options_initialize")
    GET(options_set_target_env, "shaderc_compile_options_set_target_env")
    GET(options_set_target_spirv, "shaderc_compile_options_set_target_spirv")
    GET(compile_into_spv, "shaderc_compile_into_spv")
    GET(result_get_compilation_status, "shaderc_result_get_compilation_status")
    GET(result_get_length, "shaderc_result_get_length")
    GET(result_get_bytes, "shaderc_result_get_bytes")
    GET(result_get_error_message, "shaderc_result_get_error_message")
    GET(result_release, "shaderc_result_release")
#undef GET
    g_compiler = S.compiler_initialize();
    g_options = S.options_initialize();
    if (!g_compiler || !g_options) { snprintf(g_load_error, sizeof g_load_error, "shaderc initialisation failed"); return FALSE; }
    S.options_set_target_env(g_options, SHC_ENV_VULKAN, SHC_ENV_VULKAN_1_3);
    S.options_set_target_spirv(g_options, SHC_SPIRV_1_6);
    return TRUE;
}

__declspec(dllexport) void smo_spirv_free(uint32_t *words) { free(words); }

__declspec(dllexport) int smo_glsl_to_spirv(int stage, const char *src, uint32_t **words, size_t *count, char *log, size_t log_size) {
    int ok = 0;
    if (log && log_size) log[0] = 0;
    *words = NULL; *count = 0;
    if (!InitOnceExecuteOnce(&g_once, init_once, NULL, NULL)) { if (log) snprintf(log, log_size, "%s", g_load_error); return 0; }
    int kind = stage == 0 ? SHC_VERTEX : stage == 1 ? SHC_FRAGMENT : stage == 2 ? SHC_GEOMETRY : stage == 5 ? SHC_COMPUTE : -1;
    if (kind < 0) { if (log) snprintf(log, log_size, "stage %d is not vertex (0), fragment (1), geometry (2) or compute (5)", stage); return 0; }
    shc_result r = S.compile_into_spv(g_compiler, src, strlen(src), kind, "nvn_stage", "main", g_options);
    if (!r) { if (log) snprintf(log, log_size, "shaderc_compile_into_spv returned no result"); return 0; }
    const char *msg = S.result_get_error_message(r);
    if (msg && msg[0] && log) snprintf(log, log_size, "%s", msg);
    if (S.result_get_compilation_status(r) == 0) {                    /* shaderc_compilation_status_success */
        size_t n = S.result_get_length(r) / 4;
        if (n) {
            *words = malloc(n * sizeof(uint32_t));
            if (*words) { memcpy(*words, S.result_get_bytes(r), n * 4); *count = n; ok = 1; }
        }
    }
    S.result_release(r);
    return ok;
}
