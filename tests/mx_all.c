/* mx_all.c - every shader stage of the game through the translator, offline (tests/check_all_shaders.py).
 *
 * Reads the stage files tests/check_all_shaders.py writes (build\shaders_all\*.mxs: one NVN stage each, taken out of the
 * game's shader containers), runs mx_translate on each with the Vulkan back end's settings (target 1), compiles the text
 * to SPIR-V with the renderer's own compiler (smo_glslang.dll, so run from build\bin), and counts: translated and
 * compiled, not translated (by instruction or feature), refused by the compiler. Prints the first stages of each kind of
 * failure and a summary; exit status = failures.
 *
 *   mx_all.exe <folder of .mxs> [--list]       (--list: one line per stage)
 *
 * Stage file (little endian): "MXS1", u32 stage (0 vertex, 1 fragment, 2 geometry, 3 tess control, 4 tess evaluation,
 * 5 compute), u32 code bytes, u32 constant data bytes, u32 work group x, y, z, u32 shared bytes, u32 local bytes, the
 * code (a graphics stage: program header + instructions; compute: instructions), the constant data (constant buffer 1).
 * A geometry stage is translated and compiled for each input primitive class a draw can give it (points, lines,
 * triangles) and counts as good when all three are. Tessellation stages are counted, not translated: the renderer has
 * none (the game has none either). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <windows.h>
#include "mx_translate.h"

typedef int (*GlslFn)(int, const char *, uint32_t **, size_t *, char *, size_t);
typedef void (*FreeFn)(uint32_t *);

typedef struct { char what[80]; int count; char first[3][MAX_PATH]; } Fail;
static Fail g_fail[256];
static int g_nfail;
static void note(const char *what, const char *file) {
    int i;
    for (i = 0; i < g_nfail && strcmp(g_fail[i].what, what); i++) {}
    if (i == g_nfail) { if (g_nfail == 256) i = 255; else { g_nfail++; snprintf(g_fail[i].what, sizeof g_fail[i].what, "%s", what); } }
    if (g_fail[i].count < 3) snprintf(g_fail[i].first[g_fail[i].count], MAX_PATH, "%s", file);
    g_fail[i].count++;
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: mx_all.exe <folder of .mxs> [--list]\n"); return 2; }
    int list = argc > 2 && !strcmp(argv[2], "--list");
    setvbuf(stdout, NULL, _IONBF, 0);                  /* a stage that crashes the translator: its name is out already */
    HMODULE m = LoadLibraryA("smo_glslang.dll");
    GlslFn glsl = m ? (GlslFn)GetProcAddress(m, "smo_glsl_to_spirv") : NULL;
    FreeFn spv_free = m ? (FreeFn)GetProcAddress(m, "smo_spirv_free") : NULL;
    if (!glsl || !spv_free) { printf("smo_glslang.dll not found (run from build\\bin)\n"); return 2; }
    char pat[MAX_PATH];
    snprintf(pat, sizeof pat, "%s\\*.mxs", argv[1]);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) { printf("no .mxs files in %s\n", argv[1]); return 2; }
    int total = 0, ok = 0, by_stage[6] = {0}, ok_stage[6] = {0}, skipped = 0;
    do {
        char path[MAX_PATH];
        snprintf(path, sizeof path, "%s\\%s", argv[1], fd.cFileName);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        uint32_t hd[9];
        uint8_t *code = NULL, *cdata = NULL;
        int good = fread(hd, 4, 9, f) == 9 && hd[0] == 0x3153584d && hd[1] < 6 && hd[2] && hd[2] < (64u << 20) && hd[3] < (16u << 20);
        if (good) {
            code = malloc(hd[2]); cdata = hd[3] ? malloc(hd[3]) : NULL;
            good = code && fread(code, 1, hd[2], f) == hd[2] && (!hd[3] || (cdata && fread(cdata, 1, hd[3], f) == hd[3]));
        }
        fclose(f);
        if (!good) { note("unreadable stage file", fd.cFileName); free(code); free(cdata); total++; continue; }
        int stage = (int)hd[1];
        total++; by_stage[stage]++;
        if (stage == 3 || stage == 4) { skipped++; note("tessellation stage (the renderer has none)", fd.cFileName); free(code); free(cdata); continue; }
        /* a geometry stage takes the draw's input primitive: translated and compiled for points, lines and triangles */
        int good_all = 1, variants = stage == 2 ? 3 : 1;
        if (list) printf("%s: ", fd.cFileName);
        for (int var = 0; var < variants && good_all; var++) {
            MxIn in;
            MxOut out;
            memset(&in, 0, sizeof in); memset(&out, 0, sizeof out);
            in.stage = stage; in.code = code; in.code_len = (int)hd[2]; in.cdata = cdata; in.cdata_size = hd[3];
            in.target = 1; in.binding_base = 40; in.ssbo_base = 60; in.tex_base = 40; in.image_base = 72; in.wmask_binding = 78;
            in.local_size[0] = hd[4]; in.local_size[1] = hd[5]; in.local_size[2] = hd[6]; in.smem_bytes = hd[7]; in.lmem_bytes = hd[8];
            in.gs_in_prim = var;
            char what[80];
            if (!mx_translate(&in, &out) || !out.glsl) {
                snprintf(what, sizeof what, "not translated: %s", out.op[0] ? out.op : out.why);
                note(what, fd.cFileName);
                if (list) printf("NOT TRANSLATED (%s) ", out.why);
                good_all = 0;
            } else {
                uint32_t *words = NULL;
                size_t count = 0;
                char log[400];
                if (!glsl(stage, out.glsl, &words, &count, log, sizeof log)) {
                    char *nl = strchr(log, '\n');
                    if (nl) *nl = 0;
                    snprintf(what, sizeof what, "compiler: %.60s", log[0] ? log : "(no message)");
                    note(what, fd.cFileName);
                    if (list) printf("COMPILER (%s) ", log);
                    good_all = 0;
                } else if (list) printf("ok (%d instructions, %zu SPIR-V words) ", out.insns, count);
                if (words) spv_free(words);
                free(out.glsl);
            }
        }
        if (good_all) { ok++; ok_stage[stage]++; }
        if (list) printf("\n");
        free(code); free(cdata);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    static const char *names[6] = {"vertex", "fragment", "geometry", "tess control", "tess evaluation", "compute"};
    printf("stages: %d (", total);
    for (int s = 0; s < 6; s++) if (by_stage[s]) printf("%s%s %d of %d ok", s ? ", " : "", names[s], ok_stage[s], by_stage[s]);
    printf(")\ntranslated and compiled: %d of %d; failures: %d kind(s)\n", ok, total, g_nfail);
    for (int i = 0; i < g_nfail; i++) {
        printf("  %5d  %s   e.g. %s%s%s%s%s\n", g_fail[i].count, g_fail[i].what, g_fail[i].first[0], g_fail[i].count > 1 ? ", " : "", g_fail[i].count > 1 ? g_fail[i].first[1] : "",
               g_fail[i].count > 2 ? ", " : "", g_fail[i].count > 2 ? g_fail[i].first[2] : "");
    }
    return total - ok;
}
