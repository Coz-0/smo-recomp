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
#ifdef _WIN32
#include <windows.h>
#define GLSL_LIB "smo_glslang.dll"
#else
#include "win32_posix.h"
#ifdef __APPLE__
#define GLSL_LIB "./libsmo_glslang.dylib"
#else
#define GLSL_LIB "./libsmo_glslang.so"
#endif
#endif
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
    /* --hashes: the renderer's own bindings (nvn_vk_pipelines.c vstage_build: 40 per stage) and one line per stage with the
     * hash its SPIR-V file is named by there (module_from_glsl), to see which files a run would find already made */
    int hashes = argc > 2 && !strcmp(argv[2], "--hashes");
    /* --spv <cache folder> [<k> <n>]: the shader precompilation of scripts/setup.py. Each stage with the renderer's
     * bindings, compiled and written as <cache folder>/spv/<hash>.spv when not there yet: the file the renderer reads in
     * place of compiling (module_from_glsl). <k> <n>: only the stages k, k + n, ... (one process per processor). */
    const char *spv_dir = argc > 3 && !strcmp(argv[2], "--spv") ? argv[3] : NULL;
    int part = argc > 5 && spv_dir ? atoi(argv[4]) : 0, parts = argc > 5 && spv_dir ? atoi(argv[5]) : 1, seen = -1, written = 0, had = 0;
    if (spv_dir) hashes = 2;
    setvbuf(stdout, NULL, _IONBF, 0);                  /* a stage that crashes the translator: its name is out already */
    HMODULE m = LoadLibraryA(GLSL_LIB);
    GlslFn glsl = m ? (GlslFn)GetProcAddress(m, "smo_glsl_to_spirv") : NULL;
    FreeFn spv_free = m ? (FreeFn)GetProcAddress(m, "smo_spirv_free") : NULL;
    if (!glsl || !spv_free) { printf("smo_glslang.dll not found (run from build\\bin)\n"); return 2; }
    char pat[MAX_PATH];
    snprintf(pat, sizeof pat, "%s\\*.mxs", argv[1]);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) { printf("no .mxs files in %s\n", argv[1]); return 2; }
    int total = 0, ok = 0, by_stage[6] = {0}, ok_stage[6] = {0}, skipped = 0;
    LARGE_INTEGER qf, q0, q1;                         /* time spent translating and compiling, per stage kind */
    double t_tr[6] = {0}, t_gl[6] = {0};
    QueryPerformanceFrequency(&qf);
    do {
        char path[MAX_PATH];
        snprintf(path, sizeof path, "%s\\%s", argv[1], fd.cFileName);
        if (spv_dir && parts > 1 && ++seen % parts != part) continue;
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
            if (hashes) {
                unsigned b = 40u * (unsigned)stage;
                in.binding_base = b; in.ssbo_base = b + 20; in.tex_base = b; in.image_base = b + 32; in.wmask_binding = b + 38;
                if (stage == 5) { in.binding_base = 0; in.ssbo_base = 20; in.tex_base = 0; in.image_base = 32; in.wmask_binding = 38; in.track_writes = 1; }
            }
            in.local_size[0] = hd[4]; in.local_size[1] = hd[5]; in.local_size[2] = hd[6]; in.smem_bytes = hd[7]; in.lmem_bytes = hd[8];
            in.gs_in_prim = var;
            char what[80];
            QueryPerformanceCounter(&q0);
            int translated = mx_translate(&in, &out) && out.glsl;
            QueryPerformanceCounter(&q1);
            t_tr[stage] += (double)(q1.QuadPart - q0.QuadPart) * 1000.0 / (double)qf.QuadPart;
            if (!translated) {
                snprintf(what, sizeof what, "not translated: %s", out.op[0] ? out.op : out.why);
                note(what, fd.cFileName);
                if (list) printf("NOT TRANSLATED (%s) ", out.why);
                good_all = 0;
            } else if (hashes) {
                uint64_t h = 0xcbf29ce484222325ULL ^ (uint64_t)stage ^ 0x5601;
                for (const char *q = out.glsl; *q; q++) h = (h ^ (uint8_t)*q) * 0x100000001B3ULL;
                if (hashes == 1) {
                    printf("glsl %d %016llx %s\n", stage, (unsigned long long)h, fd.cFileName);
                    const char *gd = getenv("MX_GLSL_DIR");          /* also the text itself, as <stage file>.glsl */
                    if (gd) { char gp[MAX_PATH]; snprintf(gp, sizeof gp, "%s/%s.glsl", gd, fd.cFileName); FILE *g = fopen(gp, "wb"); if (g) { fputs(out.glsl, g); fclose(g); } }
                }
                else {
                    char sp[MAX_PATH];
                    snprintf(sp, sizeof sp, "%s/spv/%016llx.spv", spv_dir, (unsigned long long)h);
                    FILE *have = fopen(sp, "rb");
                    if (have) { fclose(have); had++; }
                    else {
                        uint32_t *words = NULL;
                        size_t count = 0;
                        char log[400];
                        if (glsl(stage, out.glsl, &words, &count, log, sizeof log)) {
                            char tmp[MAX_PATH + 16];
                            snprintf(tmp, sizeof tmp, "%s.%d", sp, part);   /* written whole, then renamed: a reader never sees part of one */
                            FILE *o = fopen(tmp, "wb");
                            int ok = o && fwrite(words, 4, count, o) == count;
                            if (o && fclose(o)) ok = 0;
                            if (ok && MoveFileExA(tmp, sp, MOVEFILE_REPLACE_EXISTING)) written++; else DeleteFileA(tmp);
                            spv_free(words);
                        } else good_all = 0;
                    }
                }
                free(out.glsl);
            } else {
                uint32_t *words = NULL;
                size_t count = 0;
                char log[400];
                QueryPerformanceCounter(&q0);
                int compiled = glsl(stage, out.glsl, &words, &count, log, sizeof log);
                QueryPerformanceCounter(&q1);
                t_gl[stage] += (double)(q1.QuadPart - q0.QuadPart) * 1000.0 / (double)qf.QuadPart;
                if (!compiled) {
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
    if (spv_dir) printf("SPIR-V files: %d written, %d already there\n", written, had);
    printf("time, ms (translator / GLSL to SPIR-V):");
    for (int s = 0; s < 6; s++) if (by_stage[s]) printf(" %s %.0f / %.0f", names[s], t_tr[s], t_gl[s]);
    printf("\n");
    for (int i = 0; i < g_nfail; i++) {
        printf("  %5d  %s   e.g. %s%s%s%s%s\n", g_fail[i].count, g_fail[i].what, g_fail[i].first[0], g_fail[i].count > 1 ? ", " : "", g_fail[i].count > 1 ? g_fail[i].first[1] : "",
               g_fail[i].count > 2 ? ", " : "", g_fail[i].count > 2 ? g_fail[i].first[2] : "");
    }
    return total - ok;
}
