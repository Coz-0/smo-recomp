/* nn::pl: the console's shared fonts, read from plain TrueType / OpenType files on the PC.
 *
 * The game draws with them in one place met so far: the map screen's legend and travel-guide pages (the Standard font;
 * 189 glyphs, 61 characters, when the map first opens; the other types draw nothing in English). For each type, in
 * this order:
 *   1. the console's own font, from your dump: %SMO_FONTS% or ..\sysdata\fonts (setup's import step puts it there);
 *   2. a free font you put in the repository's deps\fonts (Noto Sans, Noto Sans SC / TC / KR, .ttf or .otf);
 *   3. Windows' own font for that script, which every Windows PC has, so the game always starts.
 * The log line says which file each type got. The Nintendo extension fonts (button icons and symbols) have no free
 * equivalent: their fallback is the Standard one's.
 *
 * SharedFontType: 0 Standard, 1 ChineseSimple, 2 ChineseSimpleExtension, 3 ChineseTraditional,
 *                 4 Korean, 5 NintendoExtension (6: its second file). SharedFontLoadState: 0 Loading, 1 Loaded. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "runtime.h"
#include "hostrt.h"

#define A0 (c->x[0])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define FONT_TYPES 7

static const char *const console_files[FONT_TYPES] = {
    "nintendo_udsg-r_std_003.ttf", "nintendo_udsg-r_org_zh-cn_003.ttf", "nintendo_udsg-r_ext_zh-cn_003.ttf",
    "nintendo_udjxh-db_zh-tw_003.ttf", "nintendo_udsg-r_ko_003.ttf", "nintendo_ext_003.ttf", "nintendo_ext2_003.ttf",
};
/* free fonts in deps\fonts, by script: a few spellings each (static or variable builds, .ttf or .otf) */
static const char *const free_files[FONT_TYPES][4] = {
    {"NotoSans-Regular.ttf", "NotoSans-Regular.otf", "NotoSans[wdth,wght].ttf", NULL},
    {"NotoSansSC-Regular.ttf", "NotoSansSC-Regular.otf", "NotoSansSC[wght].ttf", NULL},
    {"NotoSansSC-Regular.ttf", "NotoSansSC-Regular.otf", "NotoSansSC[wght].ttf", NULL},
    {"NotoSansTC-Regular.ttf", "NotoSansTC-Regular.otf", "NotoSansTC[wght].ttf", NULL},
    {"NotoSansKR-Regular.ttf", "NotoSansKR-Regular.otf", "NotoSansKR[wght].ttf", NULL},
    {"NotoSans-Regular.ttf", "NotoSans-Regular.otf", "NotoSans[wdth,wght].ttf", NULL},
    {"NotoSans-Regular.ttf", "NotoSans-Regular.otf", "NotoSans[wdth,wght].ttf", NULL},
};
/* Windows' own, in %WINDIR%\Fonts: Segoe UI, Microsoft YaHei, Microsoft JhengHei, Malgun Gothic */
static const char *const windows_files[FONT_TYPES] = {"segoeui.ttf", "msyh.ttc", "msyh.ttc", "msjh.ttc", "malgun.ttf", "segoeui.ttf", "segoeui.ttf"};
static struct { void *data; size_t size; int tried; } g_fonts[FONT_TYPES];
static CRITICAL_SECTION g_lock;

static int read_font(unsigned type, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *data = size > 0 ? _aligned_malloc((size_t)size + 16, 4096) : NULL;
    int ok = data && fread(data, 1, (size_t)size, f) == (size_t)size;
    fclose(f);
    if (!ok) { _aligned_free(data); return 0; }
    g_fonts[type].data = data;
    g_fonts[type].size = (size_t)size;
    return 1;
}

static void load_font(unsigned type) {
    if (type >= FONT_TYPES) return;
    EnterCriticalSection(&g_lock);
    if (!g_fonts[type].tried) {
        g_fonts[type].tried = 1;
        char path[MAX_PATH] = "";
        const char *dir = getenv("SMO_FONTS"), *from = "the console's (your dump)";
        snprintf(path, sizeof path, "%s\\%s", dir ? dir : "..\\sysdata\\fonts", console_files[type]);
        int ok = read_font(type, path);
        for (int k = 0; !ok && free_files[type][k]; k++) {
            snprintf(path, sizeof path, "..\\..\\deps\\fonts\\%s", free_files[type][k]);
            if ((ok = read_font(type, path))) from = "a free font (deps\\fonts)";
        }
        if (!ok) {
            char windir[MAX_PATH] = "C:\\Windows";
            GetWindowsDirectoryA(windir, sizeof windir);
            snprintf(path, sizeof path, "%s\\Fonts\\%s", windir, windows_files[type]);
            if ((ok = read_font(type, path))) from = "Windows' own (no dumped or free font found)";
        }
        printf("nn::pl: shared font %u %s (%s, %zu bytes)%s%s\n", type, ok ? "loaded" : "NOT FOUND", path, g_fonts[type].size,
               ok ? ": " : "", ok ? from : "");
        if (!ok) {
            printf("nn::pl: no font for shared font type %u (not in your dump, deps\\fonts or %%WINDIR%%\\Fonts): stopping\n", type);
            fflush(stdout);
            report_and_exit(7);
        }
    }
    LeaveCriticalSection(&g_lock);
}

static void pl_GetSharedFontLoadState(Ctx *c) { load_font((unsigned)A0); RET(1); }
static void pl_GetSharedFontAddress(Ctx *c) { unsigned t = (unsigned)A0; load_font(t); RET(t < FONT_TYPES ? (uint64_t)(uintptr_t)g_fonts[t].data : 0); }
static void pl_GetSharedFontSize(Ctx *c) { unsigned t = (unsigned)A0; load_font(t); RET(t < FONT_TYPES ? g_fonts[t].size : 0); }
static void nothing(Ctx *c) { (void)c; }

static const ImportDef pl_imports[] = {
    {"_ZN2nn2pl22GetSharedFontLoadStateENS0_14SharedFontTypeE", pl_GetSharedFontLoadState},
    {"_ZN2nn2pl20GetSharedFontAddressENS0_14SharedFontTypeE", pl_GetSharedFontAddress},
    {"_ZN2nn2pl17GetSharedFontSizeENS0_14SharedFontTypeE", pl_GetSharedFontSize},
    {"_ZN2nn2pl11BeginVrModeEv", nothing}, {"_ZN2nn2pl9EndVrModeEv", nothing},
};

void register_pl_imports(void) {
    InitializeCriticalSection(&g_lock);
    register_imports(pl_imports, (int)(sizeof pl_imports / sizeof pl_imports[0]));
}
