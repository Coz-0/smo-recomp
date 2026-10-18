/* smo_selftest.c - self-test of the system functions added for the game's offline features.
 *
 * SMO_IMPORT_SELFTEST=1: run right after every handler is registered (boot.c), before the game starts; each check
 * calls the handler the game would reach (host code or lifted sdk.elf code, through import_lookup) with known inputs
 * and compares the result. Prints one line per group and exits with the number of failures.
 * The web part writes the Help page's PC copy under ..\manual (scripts/setup.py step `manual`) without opening a
 * browser (SMO_WEB_NOOPEN); it is skipped, and says so, when the document was not extracted. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "runtime.h"
#include "hostrt.h"

#define P(v) ((uint64_t)(uintptr_t)(v))

static int g_bad;
/* A check whose function the game does not import is skipped, not
 * failed: call() marks it and the next check() consumes the mark; the names are listed at the end. */
static int g_skip_check, g_skipped;
static const char *g_skipped_names[64];
static int g_skipped_name_count;
static void check(const char *group, const char *what, int ok) {
    if (g_skip_check) { g_skip_check = 0; g_skipped++; return; }
    if (!ok) { g_bad++; printf("  FAILED %s: %s\n", group, what); }
}
static int call(Ctx *c, const char *name) {
    if (!import_data(name)) {
        int i = 0;
        while (i < g_skipped_name_count && strcmp(g_skipped_names[i], name)) i++;
        if (i == g_skipped_name_count && i < 64) g_skipped_names[g_skipped_name_count++] = name;
        g_skip_check = 1;
        return 0;
    }
    ImportFn f = import_lookup(name);
    if (!f) { g_bad++; printf("  FAILED: %s has no handler\n", name); return 0; }
    f(c);
    return 1;
}
static void args(Ctx *c, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) { c->x[0] = a0; c->x[1] = a1; c->x[2] = a2; c->x[3] = a3; }

static void test_libc(Ctx *c) {
    int before = g_bad;
    double ip = 0;
    memset(c->v, 0, sizeof c->v); c->v[0].f[0] = 1.5f; c->x[0] = 3;
    call(c, "ldexpf"); check("libc", "ldexpf(1.5, 3) = 12", c->v[0].f[0] == 12.0f);
    c->v[0].f[0] = 1.0f; c->x[0] = (uint64_t)(uint32_t)-2;
    call(c, "ldexpf"); check("libc", "ldexpf(1, -2) = 0.25", c->v[0].f[0] == 0.25f);
    c->v[0].df[0] = 3.75; c->x[0] = P(&ip);
    call(c, "modf"); check("libc", "modf(3.75) = 0.75, 3", c->v[0].df[0] == 0.75 && ip == 3.0);
    c->v[0].df[0] = -2.5; c->x[0] = P(&ip);
    call(c, "modf"); check("libc", "modf(-2.5) = -0.5, -2", c->v[0].df[0] == -0.5 && ip == -2.0);
    printf("  libc (ldexpf, modf): %s\n", g_bad == before ? "ok" : "FAILED");
}

static void test_os(Ctx *c) {
    int before = g_bad;
    static uint8_t ev[0x40], ev2[0x40], mq[0x60], th[0x200];
    static uint64_t mqbuf[4], out;
    args(c, P(ev), 0, 0, 0);                          /* manual clear */
    call(c, "_ZN2nn2os15InitializeEventEPNS0_9EventTypeEbNS0_14EventClearModeE");
    args(c, P(ev), 0, 0, 0); call(c, "_ZN2nn2os12TryWaitEventEPNS0_9EventTypeE"); check("os", "TryWaitEvent before the signal = false", c->x[0] == 0);
    args(c, P(ev), 0, 0, 0); call(c, "_ZN2nn2os11SignalEventEPNS0_9EventTypeE");
    args(c, P(ev), 0, 0, 0); call(c, "_ZN2nn2os12TryWaitEventEPNS0_9EventTypeE"); check("os", "TryWaitEvent after the signal = true", c->x[0] == 1);
    args(c, P(ev), 0, 0, 0); call(c, "_ZN2nn2os12TryWaitEventEPNS0_9EventTypeE"); check("os", "a manual-clear event stays signalled", c->x[0] == 1);
    args(c, P(ev2), 0, 1, 0);                         /* auto clear */
    call(c, "_ZN2nn2os15InitializeEventEPNS0_9EventTypeEbNS0_14EventClearModeE");
    args(c, P(ev2), 0, 0, 0); call(c, "_ZN2nn2os11SignalEventEPNS0_9EventTypeE");
    args(c, P(ev2), 0, 0, 0); call(c, "_ZN2nn2os12TryWaitEventEPNS0_9EventTypeE"); check("os", "auto-clear TryWaitEvent = true once", c->x[0] == 1);
    args(c, P(ev2), 0, 0, 0); call(c, "_ZN2nn2os12TryWaitEventEPNS0_9EventTypeE"); check("os", "auto-clear: then false", c->x[0] == 0);
    args(c, P(mq), P(mqbuf), 4, 0); call(c, "_ZN2nn2os22InitializeMessageQueueEPNS0_16MessageQueueTypeEPmm");
    args(c, P(mq), 42, 0, 0); call(c, "_ZN2nn2os16SendMessageQueueEPNS0_16MessageQueueTypeEm");
    args(c, P(mq), 43, 0, 0); call(c, "_ZN2nn2os16SendMessageQueueEPNS0_16MessageQueueTypeEm");
    out = 0; args(c, P(&out), P(mq), 0, 0); call(c, "_ZN2nn2os22TryReceiveMessageQueueEPmPNS0_16MessageQueueTypeE"); check("os", "TryReceiveMessageQueue: the oldest (42)", c->x[0] == 1 && out == 42);
    static const char name[] = "SelfTestThread";
    args(c, P(th), P(name), 0, 0); call(c, "_ZN2nn2os20SetThreadNamePointerEPNS0_10ThreadTypeEPKc");
    args(c, P(th), 0, 0, 0); call(c, "_ZN2nn2os20GetThreadNamePointerEPKNS0_10ThreadTypeE"); check("os", "GetThreadNamePointer = the name set", c->x[0] == P(name));
    static uint8_t th2[0x200];
    args(c, P(th2), 0, 0, 0); call(c, "_ZN2nn2os20GetThreadNamePointerEPKNS0_10ThreadTypeE");
    check("os", "an unnamed thread is Thread_0x<address>", c->x[0] && !strncmp((const char *)(uintptr_t)c->x[0], "Thread_0x", 9));
    printf("  nn::os (TryWaitEvent, TryReceiveMessageQueue, GetThreadNamePointer): %s\n", g_bad == before ? "ok" : "FAILED");
}

static void test_fs(Ctx *c) {
    int before = g_bad;
    char tmp[MAX_PATH], path[MAX_PATH];
    GetTempPathA(sizeof tmp, tmp);
    snprintf(path, sizeof path, "%ssmo-recomp-selftest.bin", tmp);
    HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, NULL);
    if (h == INVALID_HANDLE_VALUE) { check("fs", "temporary file", 0); return; }
    uint8_t data[100], back[50];
    DWORD n = 0;
    memset(data, 0xab, sizeof data);
    WriteFile(h, data, sizeof data, &n, NULL);
    LARGE_INTEGER sz;
    args(c, P(h), 10, 0, 0); call(c, "_ZN2nn2fs11SetFileSizeENS0_10FileHandleEl");
    check("fs", "SetFileSize(10) = success", c->x[0] == 0);
    check("fs", "the file is 10 bytes", GetFileSizeEx(h, &sz) && sz.QuadPart == 10);
    args(c, P(h), 50, 0, 0); call(c, "_ZN2nn2fs11SetFileSizeENS0_10FileHandleEl");
    check("fs", "SetFileSize(50): 50 bytes", c->x[0] == 0 && GetFileSizeEx(h, &sz) && sz.QuadPart == 50);
    OVERLAPPED ov = {0};
    memset(back, 0x55, sizeof back);
    ReadFile(h, back, sizeof back, &n, &ov);
    int grown_zero = n == 50;
    for (int i = 10; i < 50; i++) grown_zero &= back[i] == 0;
    check("fs", "the 10 bytes kept and the 40 new ones zero", grown_zero && back[0] == 0xab && back[9] == 0xab);
    args(c, P(h), 0, 0, 0); call(c, "_ZN2nn2fs9FlushFileENS0_10FileHandleE"); check("fs", "FlushFile = success", c->x[0] == 0);
    CloseHandle(h);
    DeleteFileA(path);
    printf("  nn::fs (SetFileSize, FlushFile): %s\n", g_bad == before ? "ok" : "FAILED");
}

static void test_cxx(Ctx *c) {
    int before = g_bad;
    static uint8_t obj[64];
    args(c, P(obj), 0, 0, 0); call(c, "_ZNSt3__114basic_iostreamIcNS_11char_traitsIcEEED2Ev");
    check("libc++", "basic_iostream destructor returns", c->x[0] == P(obj));
    printf("  libc++ (basic_iostream destructor): %s\n", g_bad == before ? "ok" : "FAILED");
}

static void test_util(Ctx *c) {
    int before = g_bad;
    /* a NintendoWare binary file: header 0x20, two pointers at 0x40, a relocation table at 0x100 with two sections:
     * section 0 (base 0: the file) covers the first pointer group, section 1 (base B, position 0x80) a pointer at 0x60 */
    uint8_t *f = _aligned_malloc(0x1000, 0x1000), *other = _aligned_malloc(0x100, 0x100);
    memset(f, 0, 0x1000);
    memcpy(f, "FRES    ", 8);
    f[8] = 3; f[9] = 5; wr16(P(f) + 0xa, 9);          /* version 9.5.3 */
    wr16(P(f) + 0xc, 0xfeff); f[0xe] = 0xc;            /* little endian, aligned to 4 KB */
    wr32(P(f) + 0x18, 0x100);                          /* relocation table offset */
    wr64(P(f) + 0x40, 0x20); wr64(P(f) + 0x48, 0);     /* an offset, and a null that stays null */
    wr64(P(f) + 0x60, 0x90);                           /* an offset into section 1 (position 0x80) */
    uint64_t t = P(f) + 0x100;
    memcpy(f + 0x100, "_RLT", 4); wr32(t + 4, 0x100); wr32(t + 8, 2);
    wr64(t + 0x10, 0); wr32(t + 0x18, 0); wr32(t + 0x1c, 0x80); wr32(t + 0x20, 0); wr32(t + 0x24, 1);
    wr64(t + 0x28, P(other)); wr32(t + 0x30, 0x80); wr32(t + 0x34, 0x80); wr32(t + 0x38, 1); wr32(t + 0x3c, 1);
    uint64_t e = t + 0x10 + 2 * 0x18;
    wr32(e, 0x40); wr16(e + 4, 1); f[e - P(f) + 6] = 2; f[e - P(f) + 7] = 0;
    wr32(e + 8, 0x60); wr16(e + 12, 1); f[e - P(f) + 14] = 1; f[e - P(f) + 15] = 0;
    args(c, P(f), 0x2020202053455246ull, 9, 5); c->x[4] = 3;
    call(c, "_ZNK2nn4util16BinaryFileHeader7IsValidEliii"); check("util", "IsValid(FRES, 9.5.3) = true", (c->x[0] & 1) == 1);
    args(c, P(f), 0x2020202053455246ull, 9, 4); c->x[4] = 0;
    call(c, "_ZNK2nn4util16BinaryFileHeader7IsValidEliii"); check("util", "IsValid with an older minor version = false", (c->x[0] & 1) == 0);
    args(c, P(f), 0x2020202053455247ull, 9, 5); c->x[4] = 3;
    call(c, "_ZNK2nn4util16BinaryFileHeader7IsValidEliii"); check("util", "IsValid with another signature = false", (c->x[0] & 1) == 0);
    args(c, t, 0, 0, 0); call(c, "_ZN2nn4util15RelocationTable8RelocateEv");
    check("util", "Relocate: offset + file, null kept, flag set", rd64(P(f) + 0x40) == P(f) + 0x20 && rd64(P(f) + 0x48) == 0 && (rd16(P(f) + 0x14) & 1));
    check("util", "Relocate: a section with a base: base + offset - position", rd64(P(f) + 0x60) == P(other) + 0x10);
    args(c, t, 0, 0, 0); call(c, "_ZN2nn4util15RelocationTable10UnrelocateEv");
    check("util", "Unrelocate gives the offsets back", rd64(P(f) + 0x40) == 0x20 && rd64(P(f) + 0x48) == 0 && rd64(P(f) + 0x60) == 0x90);
    args(c, P(f), 0, 0, 0); call(c, "_ZN2nn4util16BinaryFileHeader12SetRelocatedEb");
    check("util", "SetRelocated(false) clears the flag", (rd16(P(f) + 0x14) & 1) == 0);
    args(c, P(f), 1, 0, 0); call(c, "_ZN2nn4util16BinaryFileHeader12SetRelocatedEb");
    check("util", "SetRelocated(true) sets it", (rd16(P(f) + 0x14) & 1) == 1);
    _aligned_free(f); _aligned_free(other);
    static const char u8[] = "AB\xc3\xa9\xe2\x9c\x93";  /* A B e-acute check-mark: 7 bytes, 4 characters */
    uint16_t u16[16];
    memset(u16, 0xff, sizeof u16);
    args(c, P(u16), 16, P(u8), 7);
    call(c, "_ZN2nn4util30ConvertStringUtf8ToUtf16NativeEPtiPKci");
    check("util", "UTF-8 -> UTF-16: success, A B U+00E9 U+2713", c->x[0] == 0 && u16[0] == 'A' && u16[1] == 'B' && u16[2] == 0xe9 && u16[3] == 0x2713);
    char back[16];
    memset(back, 0x7f, sizeof back);
    args(c, P(back), 16, P(u16), 4);
    call(c, "_ZN2nn4util30ConvertStringUtf16NativeToUtf8EPciPKti");
    check("util", "UTF-16 -> UTF-8: success, the same 7 bytes", c->x[0] == 0 && !memcmp(back, u8, 7));
    printf("  nn::util (IsValid, Relocate / Unrelocate, SetRelocated, UTF-8 / UTF-16, lifted from sdk.elf): %s\n", g_bad == before ? "ok" : "FAILED");
}

static void test_diag_time_oe(Ctx *c) {
    int before = g_bad;
    static char name[256];
    args(c, P(name), sizeof name, GAME_SYM_nnMain + 4, 0);
    call(c, "_ZN2nn4diag13GetSymbolNameEPcmm");
    check("diag", "GetSymbolName(inside nnMain) = nnMain at its start", c->x[0] == GAME_SYM_nnMain && !strcmp(name, "nnMain"));
    args(c, GAME_SYM_nnMain, 0, 0, 0); call(c, "_ZN2nn4diag13GetSymbolSizeEm"); check("diag", "GetSymbolSize(nnMain) > 0", c->x[0] > 0 && c->x[0] < 0x100000);
    uint64_t frames[8], *st = (uint64_t *)(uintptr_t)(c->x[31] - 0x100);   /* two frame records on the test stack */
    st[0] = P(st + 4); st[1] = 0x7100001234; st[4] = 0; st[5] = 0x7100005678;
    c->x[29] = P(st); c->x[30] = 0x7100000aaa;
    args(c, P(frames), 8, 0, 0); call(c, "_ZN2nn4diag12GetBacktraceEPmi");
    check("diag", "GetBacktrace: the caller, then the frame chain", c->x[0] == 3 && frames[0] == 0x7100000aaa && frames[1] == 0x7100001234 && frames[2] == 0x7100005678);
    c->x[29] = c->x[30] = 0;
    call(c, "_ZN2nn4time10InitializeEv"); call(c, "_ZN2nn4time13IsInitializedEv"); check("time", "IsInitialized after Initialize", c->x[0] == 1);
    call(c, "_ZN2nn4time8FinalizeEv"); call(c, "_ZN2nn4time13IsInitializedEv"); check("time", "not after Finalize", c->x[0] == 0);
    call(c, "_ZN2nn4time10InitializeEv");             /* as the game left it */
    int smo_screenshot_orientation(void);
    args(c, 1, 0, 0, 0); call(c, "_ZN2nn2oe29SetScreenShotImageOrientationENS_5album16ImageOrientationE");
    check("oe", "SetScreenShotImageOrientation(1) kept", smo_screenshot_orientation() == 1);
    {   /* the Capture button's picture, turned 90 degrees as the game asked: 3 x 2 in, 2 x 3 out (a BMP, bottom-up BGRA) */
        int album_save_capture(const uint8_t *rgba, int w, int h, char *out_path, size_t out_cap);
        uint8_t img[3 * 2 * 4], bmp[256];
        char path[MAX_PATH] = "";
        for (int i = 0; i < 6; i++) { img[i * 4] = (uint8_t)(10 + 40 * i); img[i * 4 + 1] = 7; img[i * 4 + 2] = 9; img[i * 4 + 3] = 255; }
        int r = album_save_capture(img, 3, 2, path, sizeof path);
        FILE *f = path[0] ? fopen(path, "rb") : NULL;
        size_t n = f ? fread(bmp, 1, sizeof bmp, f) : 0;
        if (f) fclose(f);
        int32_t bw = 0, bh = 0;
        if (n >= 54) { memcpy(&bw, bmp + 18, 4); memcpy(&bh, bmp + 22, 4); }
        /* new (x, y) is old (y, h - 1 - x) for a clockwise turn: new (1, 0) = old (0, 0), new (0, 2) = old (2, 1) */
        #define PX(x, y) (bmp + 54 + ((size_t)(3 - 1 - (y)) * 2 + (x)) * 4)
        check("capture", "turned picture saved, 2 x 3", r == 0 && n == 54 + 2 * 3 * 4 && bw == 2 && bh == 3);
        check("capture", "pixels where a clockwise turn puts them", n == 54 + 24 && PX(1, 0)[2] == 10 && PX(0, 2)[2] == 10 + 40 * 5 && PX(0, 0)[2] == 10 + 40 * 3);
        #undef PX
        if (path[0]) DeleteFileA(path);
    }
    args(c, 0, 0, 0, 0); call(c, "_ZN2nn2oe29SetScreenShotImageOrientationENS_5album16ImageOrientationE");
    args(c, 1, 0, 0, 0); call(c, "_ZN2nn2oe38SetUserInactivityDetectionTimeExtendedEb");
    call(c, "_ZN2nn2oe37IsUserInactivityDetectionTimeExtendedEv"); check("oe", "inactivity time extended: on", c->x[0] == 1);
    args(c, 0, 0, 0, 0); call(c, "_ZN2nn2oe38SetUserInactivityDetectionTimeExtendedEb");
    call(c, "_ZN2nn2oe37IsUserInactivityDetectionTimeExtendedEv"); check("oe", "and off", c->x[0] == 0);
    printf("  nn::diag (symbols, backtrace), nn::time state, nn::oe (screenshot orientation, inactivity time): %s\n", g_bad == before ? "ok" : "FAILED");
}

static void test_web(Ctx *c) {
    int before = g_bad;
    const char *env = getenv("SMO_MANUAL");
    char probe[MAX_PATH];
    snprintf(probe, sizeof probe, "%s\\html-document\\htmlcontents.htdocs\\html\\USen\\index.html", env && env[0] ? env : "..\\manual");
    if (GetFileAttributesA(probe) == INVALID_FILE_ATTRIBUTES) { printf("  nn::web Help: SKIPPED (no extracted document: scripts\\setup.py --only manual)\n"); return; }
    static uint8_t arg[0x2100], ret[0x1100];
    static const uint64_t app = 0x0100000000010000ull;
    static const char doc[] = "htmlcontents.htdocs/html/USen/index.html?selftest=1";
    memset(arg, 0, sizeof arg);
    args(c, P(arg), P(&app), P(doc), 0);
    call(c, "_ZN2nn3web36ShowOfflineHtmlPageArgWithPlayReportC1ERKNS_13ApplicationIdEPKc");
    args(c, P(arg), 1, 0, 0); call(c, "_ZN2nn3web22ShowOfflineHtmlPageArg16SetFooterEnabledEb");
    args(c, P(arg), 1, 0, 0); call(c, "_ZN2nn3web22ShowOfflineHtmlPageArg17SetPointerEnabledEb");
    args(c, P(arg), 0, 0, 0); call(c, "_ZN2nn3web36ShowOfflineHtmlPageArgWithPlayReport20SetPlayReportEnabledEb");
    args(c, P(ret), 0, 0, 0); call(c, "_ZN2nn3web26OfflineHtmlPageReturnValueC1Ev");
    SetEnvironmentVariableA("SMO_WEB_NOOPEN", "1");
    _putenv("SMO_WEB_NOOPEN=1");
    args(c, P(ret), P(arg), 0, 0); call(c, "_ZN2nn3web19ShowOfflineHtmlPageEPNS0_26OfflineHtmlPageReturnValueERKNS0_22ShowOfflineHtmlPageArgE");
    check("web", "ShowOfflineHtmlPage = success", c->x[0] == 0);
    _putenv("SMO_WEB_NOOPEN=");
    args(c, P(ret), 0, 0, 0); call(c, "_ZNK2nn3web26OfflineHtmlPageReturnValue20GetOfflineExitReasonEv"); check("web", "exit reason: closed by the player (0)", (uint32_t)c->x[0] == 0);
    args(c, P(ret), 0, 0, 0); call(c, "_ZNK2nn3web26OfflineHtmlPageReturnValue10GetLastUrlEv");
    check("web", "GetLastUrl answers a URL", c->x[0] == P(ret) + 8 && !strncmp((const char *)(uintptr_t)c->x[0], "http", 4));
    char pc[MAX_PATH], js[MAX_PATH];
    snprintf(pc, sizeof pc, "%s\\html-document\\htmlcontents.htdocs\\html\\USen\\index_pc.html", env && env[0] ? env : "..\\manual");
    snprintf(js, sizeof js, "%s\\html-document\\htmlcontents.htdocs\\js\\nx_pc.js", env && env[0] ? env : "..\\manual");
    FILE *fp = fopen(pc, "rb");
    char head[4096] = {0};
    if (fp) { fread(head, 1, sizeof head - 1, fp); fclose(fp); }
    check("web", "index_pc.html loads nx_pc.js inside <head>", fp && strstr(head, "<script src=\"../../js/nx_pc.js\"></script>") && strstr(head, "<head") < strstr(head, "nx_pc.js"));
    check("web", "nx_pc.js written", GetFileAttributesA(js) != INVALID_FILE_ATTRIBUTES);
    printf("  nn::web Help (argument and return value lifted from sdk.elf, the page's PC copy): %s\n", g_bad == before ? "ok" : "FAILED");
}

/* The amiibo menu (smo_amiibo.c, smo_ui.c): its list from the game's own messages, its picture from the game's own layout
 * textures and fonts, composed over a plain grey frame. SMO_AMIIBO_SHOT=<file.png> also saves that picture. */
int amiibo_menu_count(void);
int amiibo_menu_entry(int i, uint8_t id[8], char *name, int cap);
int amiibo_menu_compose(const uint8_t *rgba, int w, int h, int sel, uint8_t *out);
int tex_write_png(const char *path, const uint8_t *rgba, int w, int h);
static void test_amiibo_menu(void) {
    int before = g_bad, n = amiibo_menu_count();
    if (!n) { printf("  amiibo menu: SKIPPED (the game's layout or message files are not in the RomFS folder)\n"); g_bad++; return; }
    uint8_t id[8];
    char name[64];
    int wedding = 0, gold = 0, mario = amiibo_menu_entry(0, id, name, sizeof name) && !memcmp(id, "\0\0\0\0\0\0\0\2", 8);
    for (int i = 0; i < n; i++) {
        if (!amiibo_menu_entry(i, id, name, sizeof name)) continue;
        int num = id[4] << 8 | id[5];
        wedding += (num == 881 && id[1] == 0) + (num == 882 && id[1] == 2) + (num == 883 && id[1] == 5);
        gold += num == 60 && id[0] == 0 && id[1] == 0;
    }
    check("amiibo menu", "more than 80 amiibo named by the game", n > 80);
    check("amiibo menu", "the first is Mario's identification block 00 00 00 00 00 00 00 02", mario);
    check("amiibo menu", "the three Wedding amiibo with model numbers 881 / 882 / 883", wedding == 3);
    check("amiibo menu", "the Gold Mario amiibo (model number 60)", gold == 1);
    int w = 1280, h = 720;
    uint8_t *bg = malloc((size_t)w * h * 4), *out = malloc((size_t)w * h * 4);
    for (size_t p = 0; p < (size_t)w * h; p++) { bg[p * 4] = bg[p * 4 + 1] = bg[p * 4 + 2] = 100; bg[p * 4 + 3] = 255; }
    int ok = bg && out && amiibo_menu_compose(bg, w, h, 0, out);
    check("amiibo menu", "a picture composed", ok);
    if (ok) {
        const uint8_t *px;
        px = out + ((size_t)700 * w + 640) * 4; check("amiibo menu", "the footer is white", px[0] == 255 && px[1] == 255 && px[2] == 255);
        px = out + ((size_t)664 * w + 640) * 4; check("amiibo menu", "the footer's line is the gold (186, 160, 0)", px[0] == 186 && px[1] == 160 && px[2] == 0);
        px = out + ((size_t)20 * w + 900) * 4;  check("amiibo menu", "the frame behind, blurred and a little darker (100 -> 92)", px[0] >= 90 && px[0] <= 94);
        int white = 0, red = 0, gold_px = 0;            /* the chosen row (Mario, the first): white bar, red cap, the footer guide in gold */
        for (int y = 160; y < 260; y++) for (int x = 40; x < 400; x++) { const uint8_t *q = out + ((size_t)y * w + x) * 4; white += q[0] > 250 && q[1] > 250 && q[2] > 250; red += q[0] > 200 && q[1] < 60 && q[2] < 60; }
        for (int y = 670; y < 710; y++) for (int x = 900; x < 1240; x++) { const uint8_t *q = out + ((size_t)y * w + x) * 4; gold_px += q[0] > 150 && q[2] < 80; }
        check("amiibo menu", "the white bar of the chosen row (over 10,000 white pixels)", white > 10000);
        check("amiibo menu", "the cap cursor (over 500 red pixels)", red > 500);
        check("amiibo menu", "the footer guide in gold (over 1,000 pixels)", gold_px > 1000);
        const char *shot = getenv("SMO_AMIIBO_SHOT");
        if (shot && shot[0]) { tex_write_png(shot, out, w, h); printf("  amiibo menu: picture saved to %s\n", shot); }
    }
    free(bg); free(out);
    printf("  amiibo menu (%d amiibo from the game's messages; picture from its layout textures and fonts): %s\n", n, g_bad == before ? "ok" : "FAILED");
}

/* hid, bcat, friends, pctl and mem functions of NintendoSDK 7.3.2 (smo_nn.c, nnhid.c, nnonline.inc). */
static void test_sdk7(Ctx *c) {
    int before = g_bad;
    uint32_t bad_handle = 0xff;                                  /* style 0xff: no vibration device */
    args(c, P(&bad_handle), 0, 0, 0);
    call(c, "_ZN2nn3hid24IsVibrationDeviceMountedERKNS0_21VibrationDeviceHandleE");
    check("hid", "IsVibrationDeviceMounted(an invalid handle) = false", (c->x[0] & 1) == 0);
    uint64_t file = 0;                                           /* DeliveryCacheFile: one pointer, null = closed */
    static const char ok_path[] = "dir_1/file.bin", no_slash[] = "file.bin", long_dir[] = "a23456789012345678901234567890123/f";
    args(c, P(&file), P(no_slash), 0, 0);
    call(c, "_ZN2nn4bcat17DeliveryCacheFile4OpenEPKc"); check("bcat", "File::Open(path without '/') = 0x27a", (uint32_t)c->x[0] == 0x27a);
    args(c, P(&file), P(long_dir), 0, 0);
    call(c, "_ZN2nn4bcat17DeliveryCacheFile4OpenEPKc"); check("bcat", "File::Open(directory of 33 characters) = 0x27a", (uint32_t)c->x[0] == 0x27a);
    args(c, P(&file), P(ok_path), 0, 0);
    call(c, "_ZN2nn4bcat17DeliveryCacheFile4OpenEPKc");
    check("bcat", "File::Open(valid path) = not mounted (0xa7a) or not found (0x47a); the file stays closed",
          ((uint32_t)c->x[0] == 0xa7a || (uint32_t)c->x[0] == 0x47a) && file == 0);
    uint64_t profile[2] = {0x0123456789abcdefull, 0};
    args(c, P(profile), 0, 0, 0);
    call(c, "_ZNK2nn7friends7Profile12GetAccountIdEv"); check("friends", "Profile::GetAccountId = the first field", c->x[0] == profile[0]);
    call(c, "_ZN2nn4pctl42ConfirmStereoVisionRestrictionConfigurableEv"); check("pctl", "ConfirmStereoVisionRestrictionConfigurable = 0", (uint32_t)c->x[0] == 0);
    call(c, "_ZN2nn4pctl53RequestStereoVisionRestrictionConfigurationPermissionEv"); check("pctl", "RequestStereoVisionRestrictionConfigurationPermission = 0", (uint32_t)c->x[0] == 0);
    call(c, "_ZN2nn4pctl26GetStereoVisionRestrictionEv"); check("pctl", "no stereo vision restriction at start", (c->x[0] & 1) == 0);
    args(c, 1, 0, 0, 0); call(c, "_ZN2nn4pctl26SetStereoVisionRestrictionEb");
    call(c, "_ZN2nn4pctl26GetStereoVisionRestrictionEv"); check("pctl", "SetStereoVisionRestriction(true) is kept", (c->x[0] & 1) == 1);
    args(c, 0, 0, 0, 0); call(c, "_ZN2nn4pctl26SetStereoVisionRestrictionEb");
    call(c, "_ZN2nn4pctl26GetStereoVisionRestrictionEv"); check("pctl", "and false again", (c->x[0] & 1) == 0);
    printf("  hid, bcat, friends, pctl (IsVibrationDeviceMounted, DeliveryCacheFile::Open(path), Profile::GetAccountId, stereo vision): %s\n",
           g_bad == before ? "ok" : "FAILED");
}

int smo_import_selftest(void) {
    Ctx *c = _aligned_malloc(sizeof *c, 16);
    uint8_t *stack = VirtualAlloc(NULL, 8 << 20, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    memset(c, 0, sizeof *c);
    c->x[31] = P(stack + (8 << 20) - 0x1000);
    c->tpidr = P(VirtualAlloc(NULL, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    printf("SMO import self-test (functions added for the offline docked game)\n");
    test_libc(c);
    test_os(c);
    test_fs(c);
    test_cxx(c);
    test_util(c);
    test_diag_time_oe(c);
    test_web(c);
    { int nfp_amiibo_selftest(Ctx *c); g_bad += nfp_amiibo_selftest(c); }   /* nnapplet.c: the NFC reader with an amiibo held to it */
    test_amiibo_menu();
    test_sdk7(c);
    if (g_skip_check) { g_skip_check = 0; g_skipped++; }
    if (g_skipped) {
        printf("  %d check(s) skipped: their functions are not imports of the game (%s):", g_skipped, GAME_VERSION);
        for (int i = 0; i < g_skipped_name_count; i++) printf("%s %s", i ? "," : "", g_skipped_names[i]);
        printf("\n");
    }
    printf("SMO import self-test: %d failure(s)\n", g_bad);
    fflush(stdout);
    return g_bad;
}
