/* Library applets and small device services, as an OFFLINE console answers them:
 * nn::web (offline HTML page, web page, YouTube, lobby), nn::swkbd::ShowKeyboard, nn::ec shop pages,
 * the amiibo settings applets, nn::nfp (an NFC reader to which the amiibo menu holds amiibo), nn::oe volume balance.
 *
 * Everything that only fills a caller structure (nn::web *Arg constructors and setters, *ReturnValue
 * constructors and getters, nn::swkbd MakePreset / Set*Text / SetInitialText / SetTextCheckCallback /
 * GetRequired*Size, InitializeKeyboardConfig) is NOT here: it is the SDK's own code, lifted (sdklift_gen.c).
 *
 * Tags: [sdk] address in binaries\13.0.4\sdk.elf, [game] address in main.elf, [doc] suyu source / public headers.
 *
 * ---- nn::web
 * [sdk] 0x3bd330 ShowOfflineHtmlPage: creates library applet 0x17, pushes the argument, waits for it, then
 * pops 0x1010 bytes into the OfflineHtmlPageReturnValue; Result 0 when that storage exists, 0x2d2 (module
 * 210, description 1) when the applet ended without one. Layout ([sdk] 0x3be5a0 constructor, [doc] suyu
 * WebCommonReturnValue): u32 exitReason @0 (constructor: 0x14 = "none"), char lastUrl[0x1000] @8,
 * u64 lastUrlSize @0x1008. ShowLobbyPage ([sdk] 0x2c41d8) is the same shape.
 * [sdk] 0x3bd154 ShowWebPage / 0x3bd2c4 ShowYouTubeVideo: the output is 0x2000 bytes of TLVs:
 * u16 count @0, u32 shim kind @4, then entries { u16 type, u16 size, u32 pad, data[size] } packed without
 * alignment ([sdk] 0x3c0ef8 walks them as offset += size + 8); type 1 = u32 exit reason, 2 = last URL,
 * 3 = u64 URL size, 9 = u8 "media player closed by completion".
 * [doc] suyu WebExitReason: 0 EndButtonPressed, 1 BackButtonPressed, 2 ExitRequested, 3 CallbackURL,
 * 4 WindowClosed, 7 ErrorDialog.
 * What happens here: no page can be shown (no browser applet; the game's offline HTML documents are in
 * its manual/legal NCA, not in the extracted RomFS), so each call answers what the applet returns when
 * the user closes the page at once: exit reason 0 (EndButtonPressed), last URL "http://localhost/" (what
 * suyu's applet without a browser frontend reports). [game] 0x710375535c ignores the Result and the value
 * of ShowOfflineHtmlPage; [game] 0x7103341e50 only acts on ShowWebPage when the exit reason is 3
 * (callback URL), which cannot happen without a network.
 *
 * ---- nn::swkbd::ShowKeyboard(String *out, const ShowKeyboardArg &arg)
 * [sdk] 0x38a390. ShowKeyboardArg: KeyboardConfig (0x4d0 bytes) @0; work buffer @0x4d0, size @0x4d8;
 * text-check work buffer @0x4e0 (the applet's dialog message goes back from there, 0x7d4 bytes); a
 * second buffer @0x4f0 / size @0x4f8 (user dictionary). KeyboardConfig fields used here ([sdk] MakePreset
 * 0x38a810, SetInitialText 0x38b130, SetTextCheckCallback 0x38b340, [doc] nnheaders swkbd): header text
 * (char16[65]) @0x24, sub text (char16[129]) @0xa6, guide text (char16[257]) @0x1a8, textMaxLength @0x3ac,
 * textMinLength @0x3b0, passwordMode @0x3b4, isUseUtf8 @0x3bd, initial text offset in the work buffer
 * @0x3c0 and length in characters @0x3c4, text-check enabled @0x3d0, callback @0x3d8.
 * String: pointer @0, buffer size in bytes @8.
 * Results: 0 with the text (UTF-16, or UTF-8 when isUseUtf8) when confirmed; 0x29f (module 159,
 * description 1) when cancelled ([sdk] 0x38a75c; [game] 0x7103300584 compares with 0x29f).
 * Text check ([sdk] 0x389e40): the callback is called as cb(textCheckWorkBuf, size_t *outMessageSize,
 * String *text) with text->bufSize temporarily set to the size of the entered text; it returns
 * [doc] TextCheckResult 0 Success, 1 ShowFailureDialog, 2 ShowConfirmDialog, with the message in
 * textCheckWorkBuf.
 * Here the applet is a small Win32 dialog (edit box, OK, Cancel) run on the calling guest thread, which
 * blocks exactly like on the console. Scripted runs must not block: SMO_SWKBD=cancel answers "cancelled",
 * SMO_SWKBD=text:<utf-8 text> answers that text, SMO_SWKBD=dialog forces the dialog; without the
 * variable the dialog is used unless an input script (SMO_SEQ / SMO_PRESS / SMO_SCRIPT) is active.
 *
 * ---- nn::nfp
 * Console modelled: the player-1 controller has an NFC reader (as a Pro Controller / right Joy-Con does).
 * An amiibo is held to it only by the amiibo menu (smo_amiibo.c, nfp_place_amiibo): the reader then finds
 * it the next time the game searches, the game mounts it and reads its tag and model information, and the
 * amiibo is taken away again once the game has read it (one touch). [sdk] 0x309900 GetState: 0 before
 * Initialize, the service's state after (1 = initialised); [sdk] 0x309a84 GetDeviceState: 5 before
 * Initialize. [doc] suyu nfc_types.h DeviceState 0 Initialized, 1 SearchingForTag, 2 TagFound,
 * 3 TagRemoved, 4 TagMounted, 5 Unavailable; nfp_result.h (module 115): DeviceNotFound 64,
 * InvalidArgument 65, WrongDeviceState 73, TagRemoved 97.
 * The transitions are suyu's (core/hle/service/nfc/common/device.cpp): StartDetection from Initialized or
 * TagRemoved to SearchingForTag; a tag in the field: TagFound, the deactivate event cleared and the
 * activate event signalled; Mount: TagFound -> TagMounted; Unmount: back to TagFound; the tag leaving the
 * field: TagRemoved (unmounted first), the activate event cleared and the deactivate event signalled;
 * StopDetection: the tag closed if there is one, then Initialized.
 * The events are attached as AutoClear ([smo sdk.elf] 0x12e220: AttachReadableHandleToSystemEvent with
 * clear mode 1). How the game uses the reader (al::NfpControllerThread, [game] 0x95efa8): AttachEvent,
 * then StartDetection; TagDetect polls GetDeviceState and waits 100 us on the activate event every 60 ms;
 * Mount(handle, ModelType 0, MountTarget 1); TagRead: GetTagInfo and GetModelInfo; Deactive: Unmount,
 * then GetDeviceState every 60 ms while it says TagFound, and StartDetection again once the tag is gone.
 * TagInfo ([doc] suyu nfc_types.h, 0x58 bytes): uuid[10] @0, uuid length @0xa, protocol u32 @0x20
 * (1 = type A), tag type u32 @0x24 (2 = type 2: an amiibo is an NTAG215). ModelInfo (0x40 bytes) as
 * the game reads it ([game] 0x960478 al::isEqualCharacterId, 0x960530 al::isEqualNumberingId): the
 * amiibo's identification block as stored on the tag, bytes 0 and 1 (game and character), 2 (variant),
 * 3 (type), the model number at 4 as a little-endian u16 (big-endian on the tag), 6 (series).
 * [sdk] 0x308814 amiibo settings applet closed without a change: Result 0x1a073 (module 115, description 208).
 * NOT VERIFIED on a console: the DeviceHandle value (8 bytes; the npad id 0 is used), and the Result of
 * calls made before Initialize (WrongDeviceState is returned). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "sdklift.h"

#define A0 (c->x[0])
#define A1 (c->x[1])
#define A2 (c->x[2])
#define A3 (c->x[3])
#define A4 (c->x[4])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define P(v) ((uint64_t)(uintptr_t)(v))

void os_init_event(uint64_t key, int autoclear);          /* os_threads.c */
void os_signal_event(uint64_t key);
void os_clear_event(uint64_t key);

/* ================================================================ nn::web */
#define WEB_RESULT_TERMINATED 0x2d2u
#define WEB_EXIT_END_BUTTON 0u
static const char g_last_url[] = "http://localhost/";

/* Input TLVs of a Show*Arg ([doc] switchbrew "Internet Browser": header u16 count, u16 pad, u32 shim kind; entries
 * u16 type, u16 size, u32 pad; type 1 initial URL, 6 document path). Only used to say in the log what was asked. */
static const char *web_arg_string(uint64_t arg, int type) {
    uint32_t count = rd16(arg), off = 8;
    for (uint32_t i = 0; i < count && off + 8 < 0x2000; i++) {
        uint32_t t = rd16(arg + off), size = rd16(arg + off + 2);
        if ((int)t == type) return (const char *)(uintptr_t)(arg + off + 8);
        off += size + 8;
    }
    return NULL;
}
static void web_log(Ctx *c, const char *what, uint64_t arg) {
    const char *url = web_arg_string(arg, 1), *doc = web_arg_string(arg, 6);
    printf("%s: \"%.200s\" cannot be shown here; answered as closed by the user (caller 0x%llx)\n", what, doc ? doc : url ? url : "",
           (unsigned long long)c->x[30] - 4);
    trace_printf("     %s \"%.200s\" -> closed (EndButtonPressed)\n", what, doc ? doc : url ? url : "");
}
static void web_fill_common(uint64_t ret) {                /* 0x1010 bytes */
    memset((void *)(uintptr_t)ret, 0, 0x1010);
    wr32(ret, WEB_EXIT_END_BUTTON);
    memcpy((void *)(uintptr_t)(ret + 8), g_last_url, sizeof g_last_url);
    wr64(ret + 0x1008, sizeof g_last_url - 1);
}
static void web_fill_tlv(uint64_t ret, uint32_t shim_kind) {   /* 0x2000 bytes */
    uint8_t *p = (uint8_t *)(uintptr_t)ret;
    uint32_t off = 8;
    memset(p, 0, 0x2000);
    wr16(ret + off, 1); wr16(ret + off + 2, 4); wr32(ret + off + 8, WEB_EXIT_END_BUTTON); off += 8 + 4;
    wr16(ret + off, 2); wr16(ret + off + 2, (uint16_t)sizeof g_last_url); memcpy(p + off + 8, g_last_url, sizeof g_last_url); off += 8 + (uint32_t)sizeof g_last_url;
    wr16(ret + off, 3); wr16(ret + off + 2, 8); wr64(ret + off + 8, sizeof g_last_url - 1);
    wr16(ret, 3);
    wr32(ret + 4, shim_kind);
}
/* Read in close4 (ShowWebPage / ShowYouTubeVideo / ShowLobbyPage stay OPEN):
 * [sdk] 0x3bd0c0 ShowWebPage and 0x3bd210 ShowYouTubeVideo: library applet 0x13, common arguments (version 0x80000) + the
 * 0x2000-byte argument, start (a failure aborts), then CommonApi::JoinAndFinishLibraryApplet reads 0x2000 bytes: Result 0,
 * or 0x2d2 when the applet left no storage.  [sdk] 0x2c4160 ShowLobbyPage: library applet 0x18, 0x1010 bytes back, same
 * Results.  The SDK makes NO connection check of its own: what comes back on a console without a network is decided
 * inside the browser applet (it shows its own error screen and returns some exit reason), and neither the SDK nor
 * [doc] suyu am/frontend/applet_web_browser.cpp shows which: suyu opens a host browser for Web (ExecuteWeb) and ends
 * Lobby at once with exit reason 0 because "Lobby Applet is not implemented".
 * What the game does with the answers: [game] 0x7103341e50 ShowWebPage: only exit reason 3 (callback URL) is acted on,
 * every other answer and every failure take the same path; [game] 0x71002ab9fc ShowLobbyPage: stores 0 when the exit
 * reason is 0x14 or 0xc and 1 for anything else -- so the unknown exit reason IS visible to the game there;
 * [game] 0x71017634cc / 0x7103341bac ShowYouTubeVideo: reads IsMediaPlayerAutoClosedByCompletion (TLV 9, absent here =
 * false). */
/* SMO port: ShowOfflineHtmlPage with the game's own offline document: SMO's Help (al::HtmlViewer::call asks for
 * "htmlcontents.htdocs/html/<region and language>/index.html<query>"). On the console the browser applet shows the page
 * full screen and the game waits until the player closes it. Here the page opens in the PC's web browser from the
 * extracted document (scripts/setup.py step `manual` -> build\manual, read as ..\manual from the exe's folder, or
 * SMO_MANUAL=<folder>), and the call returns at once as closed by the player.
 * The page's script uses the console browser's window.nx (footer buttons L / R / B, endApplet, play reports) before
 * anything else and stops without it; so it is shown from a copy next to it (<name>_pc.html, written once) that first
 * loads nx_pc.js (written into the document's js folder): the footer buttons answer to the keyboard (Q / E or Page Up /
 * Page Down for L / R, Escape or Backspace for B; the page's own arrow keys stay) and to a pad through the browser's
 * Gamepad API (shoulder buttons L / R, bottom face button B, right face button A, D-pad = arrow keys), endApplet closes
 * the tab, play reports do nothing. A file URL with a query cannot be opened by name, so a small page in %TEMP%
 * forwards to it. Without the document the page is answered as closed, as before. */
static const char g_nx_pc_js[] =
    "/* smo-recomp: window.nx of the console's browser applet, for SMO's Help in a PC browser (written by the runtime) */\n"
    "(function () {\n"
    "  var keys = {};\n"
    "  function press(b) { if (keys[b]) keys[b](); }\n"
    "  function arrow(code) { if (document.onkeydown) document.onkeydown({ keyCode: code, preventDefault: function () {} }); }\n"
    "  document.addEventListener('keydown', function (e) {\n"
    "    var k = e.key;\n"
    "    if (k === 'Escape' || k === 'Backspace') { e.preventDefault(); press('B'); }\n"
    "    else if (k === 'q' || k === 'Q' || k === 'PageUp') { e.preventDefault(); press('L'); }\n"
    "    else if (k === 'e' || k === 'E' || k === 'PageDown') { e.preventDefault(); press('R'); }\n"
    "  }, true);\n"
    "  var was = [];\n"
    "  function poll() {\n"
    "    var pads = navigator.getGamepads ? navigator.getGamepads() : [];\n"
    "    for (var i = 0; i < pads.length; i++) {\n"
    "      var p = pads[i];\n"
    "      if (!p || p.mapping !== 'standard') continue;\n"
    "      var w = was[i] || [];\n"
    "      for (var b = 0; b < p.buttons.length && b < 16; b++) {\n"
    "        var on = p.buttons[b].pressed;\n"
    "        if (on && !w[b]) {\n"
    "          if (b === 0) press('B'); else if (b === 4) press('L'); else if (b === 5) press('R');\n"
    "          else if (b === 1 && document.activeElement && document.activeElement.click) document.activeElement.click();\n"
    "          else if (b === 12) arrow(38); else if (b === 13) arrow(40); else if (b === 14) arrow(37); else if (b === 15) arrow(39);\n"
    "        }\n"
    "        w[b] = on;\n"
    "      }\n"
    "      was[i] = w;\n"
    "    }\n"
    "    requestAnimationFrame(poll);\n"
    "  }\n"
    "  requestAnimationFrame(poll);\n"
    "  window.nx = {\n"
    "    footer: { setAssign: function (b, label, fn) { keys[b] = fn; }, unsetAssign: function (b) { delete keys[b]; } },\n"
    "    playReport: { setCounterSetIdentifier: function () {}, incrementCounter: function () {} },\n"
    "    endApplet: function () {\n"
    "      window.close();\n"
    "      setTimeout(function () { document.body.innerHTML = '<p style=\"color:#fff;font:20px sans-serif;padding:2em\">Close this tab to return to the game.</p>'; }, 300);\n"
    "    }\n"
    "  };\n"
    "})();\n";
static int write_if_differs(const char *path, const char *data, size_t n) {
    FILE *f = fopen(path, "rb");
    if (f) {
        char *old = malloc(n + 1);
        size_t got = old ? fread(old, 1, n + 1, f) : 0;
        fclose(f);
        int same = old && got == n && !memcmp(old, data, n);
        free(old);
        if (same) return 1;
    }
    f = fopen(path, "wb");
    if (!f) return 0;
    int ok = fwrite(data, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}
static void url_append(char *url, size_t cap, const char *s) {      /* a path as a file URL: forward slashes, %XX escapes */
    size_t n = strlen(url);
    for (; *s && n + 4 < cap; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '\\') url[n++] = '/';
        else if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || strchr("/:._-~", ch)) url[n++] = (char)ch;
        else n += (size_t)snprintf(url + n, cap - n, "%%%02X", ch);
    }
    url[n] = 0;
}
static int web_open_offline(const char *doc) {
    char root[MAX_PATH], path[MAX_PATH], full[MAX_PATH], pc[MAX_PATH], js[MAX_PATH];
    const char *env = getenv("SMO_MANUAL"), *q = strpbrk(doc, "?#");
    size_t plen = q ? (size_t)(q - doc) : strlen(doc);
    snprintf(root, sizeof root, "%s", env && env[0] ? env : "..\\manual");
    snprintf(path, sizeof path, "%s\\html-document\\%.*s", root, (int)plen, doc);
    for (char *p = path; *p; p++) if (*p == '/') *p = '\\';
    if (!GetFullPathNameA(path, sizeof full, full, NULL) || GetFileAttributesA(full) == INVALID_FILE_ATTRIBUTES) {
        printf("nn::web::ShowOfflineHtmlPage: %s not found (run scripts\\setup.py --only manual)\n", full);
        return 0;
    }
    /* nx_pc.js: <document root>\htmlcontents.htdocs\js\nx_pc.js, the page's own js folder when the path has that form */
    char *html = strstr(full, "\\html\\");
    snprintf(js, sizeof js, "%.*s\\js\\nx_pc.js", html ? (int)(html - full) : 0, full);
    if (!html || !write_if_differs(js, g_nx_pc_js, sizeof g_nx_pc_js - 1)) { printf("nn::web::ShowOfflineHtmlPage: cannot write %s\n", js); return 0; }
    /* <name>_pc.html: the page with nx_pc.js loaded first */
    FILE *f = fopen(full, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *src = malloc((size_t)size + 1), *out = malloc((size_t)size + 256);
    if (!src || !out || fread(src, 1, (size_t)size, f) != (size_t)size) { fclose(f); free(src); free(out); return 0; }
    fclose(f);
    src[size] = 0;
    const char *tag = "<script src=\"../../js/nx_pc.js\"></script>";
    char *head = NULL;
    for (char *p = src; *p && !head; p++) if (!_strnicmp(p, "<head", 5)) head = strchr(p, '>');
    size_t at = head ? (size_t)(head + 1 - src) : 0, n = 0;
    memcpy(out, src, at); n = at;
    n += (size_t)sprintf(out + n, "%s", tag);
    memcpy(out + n, src + at, (size_t)size - at); n += (size_t)size - at;
    snprintf(pc, sizeof pc, "%.*s_pc.html", (int)(strlen(full) - 5), full);       /* ...\index.html -> ...\index_pc.html */
    int ok = strlen(full) > 5 && !_stricmp(full + strlen(full) - 5, ".html") && write_if_differs(pc, out, n);
    free(src); free(out);
    if (!ok) { printf("nn::web::ShowOfflineHtmlPage: cannot write %s\n", pc); return 0; }
    /* the forwarding page, then the default browser */
    char url[4096] = "file:///", fwd[MAX_PATH], tmp[MAX_PATH], page[8192];
    url_append(url, sizeof url, pc);
    if (q) snprintf(url + strlen(url), sizeof url - strlen(url), "%s", q);
    if (!GetTempPathA(sizeof tmp, tmp)) return 0;
    snprintf(fwd, sizeof fwd, "%ssmo-recomp-help.html", tmp);
    int pn = snprintf(page, sizeof page, "<!doctype html><meta charset=\"utf-8\"><meta http-equiv=\"refresh\" content=\"0; url=%s\">"
                      "<title>Super Mario Odyssey - Help</title><a href=\"%s\">Help</a>\n", url, url);
    if (pn <= 0 || !write_if_differs(fwd, page, (size_t)pn)) return 0;
    if (getenv("SMO_WEB_NOOPEN")) { printf("nn::web::ShowOfflineHtmlPage: \"%s\" ready as %s (not opened: SMO_WEB_NOOPEN)\n", doc, url); return 1; }    /* self-test */
    HINSTANCE r = ShellExecuteA(NULL, "open", fwd, NULL, NULL, SW_SHOWNORMAL);
    printf("nn::web::ShowOfflineHtmlPage: \"%s\" opened in the web browser (%s)%s\n", doc, url, (INT_PTR)r > 32 ? "" : ": FAILED");
    return (INT_PTR)r > 32;
}
static void web_ShowOfflineHtmlPage(Ctx *c) {
    uint64_t ret = A0;
    const char *doc = web_arg_string(A1, 6);
    if (!doc || !web_open_offline(doc)) web_log(c, "nn::web::ShowOfflineHtmlPage", A1);
    web_fill_common(ret);
    RET(0);
}
static void web_ShowLobbyPage(Ctx *c) { uint64_t ret = A0; web_log(c, "nn::web::ShowLobbyPage", A1); web_fill_common(ret); RET(0); }
static void web_ShowWebPage(Ctx *c) { uint64_t ret = A0; web_log(c, "nn::web::ShowWebPage", A1); web_fill_tlv(ret, 5); RET(0); }     /* [doc] ShimKind::Web = 5 */
static void web_ShowYouTubeVideo(Ctx *c) { uint64_t ret = A0; web_log(c, "nn::web::ShowYouTubeVideo", A1); web_fill_tlv(ret, 5); RET(0); }

/* ================================================================ nn::ec shop pages
 * [sdk] 0x18cad0 ShowShopAddOnContentList(ApplicationId), 0x18cb10 (+ UserHandle), 0x18d250 ShowShopProductDetails,
 * 0x18d130 ShowShopProductList: void functions.  The UserHandle forms turn the handle into a Uid (account::GetUserId,
 * zero Uid on failure, 0x18cb2c); the Impl (0x1907a0, 0x191330, 0x190ff0) builds the page path and calls
 * detail::ShowShopPage (0x1933a0): library applet 0x14 with the common arguments and the page argument, start, wait,
 * 0x1010 bytes of return value read into an SDK-internal buffer (0xc860c0).  The Result is only inspected for an abort
 * (anything but success and 0x690, 0x190898); neither it nor the return value is handed to the caller.
 * So whatever the shop applet does on a console without a connection (it is the web applet in shop mode; [doc] suyu
 * am/frontend/applet_web_browser.cpp ExecuteShop: ends at once with exit reason 0), the GAME sees a function that
 * returns nothing and writes nothing: that is what the handler does.  Not modelled: the applet's own screen and the
 * focus messages a library applet in front causes. */
static void ec_ShowShop(Ctx *c) {
    printf("nn::ec: the game asked for a Nintendo eShop page (caller 0x%llx); there is no shop here, returning as if it was closed\n",
           (unsigned long long)c->x[30] - 4);
    trace_printf("     nn::ec::ShowShop* -> closed\n");
}

/* ================================================================ nn::oe volume balance
 * [sdk] 0x32c5b0 SetExpectedVolumeBalance(float mainApplet, float libraryApplet): one request to the applet
 * manager's audio controller (object from 0x5368a0, slot +0x20 = command 0 SetExpectedMasterVolume); a failure
 * aborts. [sdk] 0x32c620 GetExpectedVolumeBalance(float *mainApplet, float *libraryApplet): two requests
 * (slot +0x28 = command 1 GetMainAppletExpectedMasterVolume, slot +0x30 = command 2
 * GetLibraryAppletExpectedMasterVolume), each failure aborts. Nothing else happens on the SDK side.
 * The applet manager's side (not in the dump), after [doc] suyu am/service/audio_controller.cpp/.h: it keeps
 * the two values clamped to [0, 1]; initial values main 0.25, library 1.0; the reference does nothing else
 * with them. What the console's applet manager does to the SOUND with them while a library applet is in front
 * is not in any source available here: NOT implemented, NOT verified. [game] 0x71037552fc saves them around
 * the offline HTML page and restores them afterwards. */
static float g_volume_main = 0.25f, g_volume_library = 1.0f;
static float clamp01(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }
static void oe_GetExpectedVolumeBalance(Ctx *c) {
    memcpy((void *)(uintptr_t)A0, &g_volume_main, 4);
    memcpy((void *)(uintptr_t)A1, &g_volume_library, 4);
}
static void oe_SetExpectedVolumeBalance(Ctx *c) { g_volume_main = clamp01(c->v[0].f[0]); g_volume_library = clamp01(c->v[1].f[0]); }

/* ================================================================ nn::swkbd::ShowKeyboard */
#define SWKBD_RESULT_CANCELED 0x29fu
#define KC_HEADER 0x24
#define KC_SUB 0xa6
#define KC_GUIDE 0x1a8
#define KC_MAXLEN 0x3ac
#define KC_MINLEN 0x3b0
#define KC_PASSWORD 0x3b4
#define KC_UTF8 0x3bd
#define KC_INITIAL_OFFSET 0x3c0
#define KC_INITIAL_LENGTH 0x3c4
#define KC_CHECK_ENABLED 0x3d0
#define KC_CHECK_CALLBACK 0x3d8
#define ARG_WORKBUF 0x4d0
#define ARG_CHECKBUF 0x4e0
#define SWKBD_MAX_CHARS 500                               /* [sdk] 0x38b150: SetInitialText stops at 0x1f4 characters */

typedef struct {
    const wchar_t *header, *sub, *guide, *initial;
    int max_len, min_len, password;
    wchar_t text[SWKBD_MAX_CHARS + 2];
} KbdDialog;

#define IDC_EDIT 1001
#define IDC_INFO 1002
static INT_PTR CALLBACK kbd_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    KbdDialog *k = (KbdDialog *)GetWindowLongPtrW(dlg, GWLP_USERDATA);
    switch (msg) {
    case WM_INITDIALOG: {
        k = (KbdDialog *)lp;
        SetWindowLongPtrW(dlg, GWLP_USERDATA, (LONG_PTR)k);
        wchar_t info[600];
        _snwprintf(info, 599, L"%ls%ls%ls%ls%ls", k->header, k->header[0] && k->sub[0] ? L"\r\n" : L"", k->sub,
                   (k->header[0] || k->sub[0]) && k->guide[0] ? L"\r\n" : L"", k->guide);
        info[599] = 0;
        SetDlgItemTextW(dlg, IDC_INFO, info[0] ? info : L"Enter text");
        SendDlgItemMessageW(dlg, IDC_EDIT, EM_SETLIMITTEXT, (WPARAM)k->max_len, 0);
        if (k->password) SendDlgItemMessageW(dlg, IDC_EDIT, EM_SETPASSWORDCHAR, (WPARAM)L'*', 0);
        SetDlgItemTextW(dlg, IDC_EDIT, k->initial);
        SendDlgItemMessageW(dlg, IDC_EDIT, EM_SETSEL, 0, -1);
        SetForegroundWindow(dlg);
        SetFocus(GetDlgItem(dlg, IDC_EDIT));
        return FALSE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            GetDlgItemTextW(dlg, IDC_EDIT, k->text, SWKBD_MAX_CHARS + 1);
            if ((int)wcslen(k->text) < k->min_len) { MessageBeep(MB_ICONWARNING); return TRUE; }
            EndDialog(dlg, 1);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, 0); return TRUE; }
        break;
    case WM_CLOSE: EndDialog(dlg, 0); return TRUE;
    }
    return FALSE;
}
static uint8_t *tmpl_align(uint8_t *p, int a) { while ((uintptr_t)p & (uintptr_t)(a - 1)) *p++ = 0; return p; }
static uint8_t *tmpl_wstr(uint8_t *p, const wchar_t *s) { size_t n = (wcslen(s) + 1) * sizeof(wchar_t); memcpy(p, s, n); return p + n; }
static uint8_t *tmpl_item(uint8_t *p, DWORD style, int x, int y, int w, int h, int id, WORD cls, const wchar_t *text) {
    p = tmpl_align(p, 4);
    DLGITEMTEMPLATE it = {style | WS_CHILD | WS_VISIBLE, 0, (short)x, (short)y, (short)w, (short)h, (WORD)id};
    memcpy(p, &it, sizeof it); p += sizeof it;
    WORD cl[2] = {0xffff, cls};
    memcpy(p, cl, 4); p += 4;
    p = tmpl_wstr(p, text);
    *p++ = 0; *p++ = 0;                                   /* no creation data */
    return p;
}
static int kbd_run_dialog(KbdDialog *k) {
    uint8_t buf[1024], *p = buf;
    memset(buf, 0, sizeof buf);
    DLGTEMPLATE dt = {DS_MODALFRAME | DS_CENTER | DS_SETFONT | WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_SETFOREGROUND, WS_EX_TOPMOST, 4, 0, 0, 260, 96};
    memcpy(p, &dt, sizeof dt); p += sizeof dt;
    *p++ = 0; *p++ = 0; *p++ = 0; *p++ = 0;               /* no menu, default class */
    p = tmpl_wstr(p, L"Software keyboard (smo-recomp)");
    *p++ = 9; *p++ = 0;
    p = tmpl_wstr(p, L"Segoe UI");
    p = tmpl_item(p, SS_LEFT, 8, 6, 244, 34, IDC_INFO, 0x0082, L"");
    p = tmpl_item(p, ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 8, 44, 244, 14, IDC_EDIT, 0x0081, L"");
    p = tmpl_item(p, BS_DEFPUSHBUTTON | WS_TABSTOP, 144, 72, 50, 16, IDOK, 0x0080, L"OK");
    p = tmpl_item(p, BS_PUSHBUTTON | WS_TABSTOP, 202, 72, 50, 16, IDCANCEL, 0x0080, L"Cancel");
    INT_PTR r = DialogBoxIndirectParamW(GetModuleHandleW(NULL), (DLGTEMPLATE *)buf, NULL, kbd_proc, (LPARAM)k);
    if (r == -1) { printf("nn::swkbd: the text dialog could not be created (error %lu); answered as cancelled\n", GetLastError()); return 0; }
    return (int)r;
}
static void copy_w(wchar_t *dst, uint64_t src, int max) {
    int i = 0;
    for (; i < max; i++) { uint16_t ch = rd16(src + (uint64_t)i * 2); if (!ch) break; dst[i] = ch; }
    dst[i] = 0;
}
/* How this call is answered: 0 dialog, 1 cancel, 2 fixed text. */
static int kbd_mode(wchar_t *fixed) {
    const char *v = getenv("SMO_SWKBD");
    if (v && strncmp(v, "text:", 5) == 0) { MultiByteToWideChar(CP_UTF8, 0, v + 5, -1, fixed, SWKBD_MAX_CHARS + 1); fixed[SWKBD_MAX_CHARS] = 0; return 2; }
    if (v && strcmp(v, "cancel") == 0) return 1;
    if (v && strcmp(v, "dialog") == 0) return 0;
    if (getenv("SMO_SEQ") || getenv("SMO_PRESS") || getenv("SMO_SCRIPT")) return 1;
    return 0;
}
/* Writes `text` into the caller's String the way the applet's storage is copied ([sdk] 0x389ff4):
 * at most bufSize bytes, always terminated. Returns the number of bytes used including the terminator. */
static uint64_t kbd_store(uint64_t string, const wchar_t *text, int utf8) {
    uint64_t ptr = rd64(string), cap = rd64(string + 8);
    if (!ptr || !cap) return 0;
    if (utf8) {
        char tmp[SWKBD_MAX_CHARS * 4 + 4];
        int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, tmp, (int)sizeof tmp, NULL, NULL);
        if (n <= 0) { n = 1; tmp[0] = 0; }
        while ((uint64_t)n > cap) { n--; while (n > 1 && ((uint8_t)tmp[n - 1] & 0xc0) == 0x80) n--; if (n > 0) n--; tmp[n] = 0; n++; }
        memcpy((void *)(uintptr_t)ptr, tmp, (size_t)n);
        return (uint64_t)n;
    }
    uint64_t chars = wcslen(text);
    if ((chars + 1) * 2 > cap) chars = cap / 2 ? cap / 2 - 1 : 0;
    if (cap < 2) return 0;
    for (uint64_t i = 0; i < chars; i++) wr16(ptr + i * 2, (uint16_t)text[i]);
    wr16(ptr + chars * 2, 0);
    return (chars + 1) * 2;
}
static void swkbd_ShowKeyboard(Ctx *c) {
    uint64_t string = A0, arg = A1;
    static KbdDialog k;                                    /* one keyboard at a time, as on the console */
    static volatile LONG busy;
    static wchar_t header[70], sub[140], guide[260], initial[SWKBD_MAX_CHARS + 2], fixed[SWKBD_MAX_CHARS + 2];
    if (InterlockedExchange(&busy, 1)) { RET(SWKBD_RESULT_CANCELED); return; }
    copy_w(header, arg + KC_HEADER, 64);
    copy_w(sub, arg + KC_SUB, 128);
    copy_w(guide, arg + KC_GUIDE, 256);
    initial[0] = 0;
    uint64_t work = rd64(arg + ARG_WORKBUF);
    int init_len = (int)rd32(arg + KC_INITIAL_LENGTH), init_off = (int)rd32(arg + KC_INITIAL_OFFSET);
    if (work && init_len > 0 && init_len <= SWKBD_MAX_CHARS && init_off >= 0) copy_w(initial, work + (uint64_t)init_off, init_len);
    int utf8 = rd8(arg + KC_UTF8) != 0;
    int max_len = (int)rd32(arg + KC_MAXLEN), min_len = (int)rd32(arg + KC_MINLEN);
    if (max_len <= 0 || max_len > SWKBD_MAX_CHARS) max_len = SWKBD_MAX_CHARS;
    if (min_len < 0 || min_len > max_len) min_len = 0;
    uint64_t callback = rd8(arg + KC_CHECK_ENABLED) ? rd64(arg + KC_CHECK_CALLBACK) : 0, check_buf = rd64(arg + ARG_CHECKBUF);
    int mode = kbd_mode(fixed);
    uint32_t result = SWKBD_RESULT_CANCELED;
    trace_printf("     nn::swkbd::ShowKeyboard header \"%ls\" guide \"%ls\" max %d min %d%s\n", header, guide, max_len, min_len, callback ? " (text check)" : "");
    for (int attempt = 0; attempt < 16; attempt++) {
        int confirmed;
        if (mode == 1) confirmed = 0;
        else if (mode == 2) { wcsncpy(k.text, fixed, (size_t)max_len); k.text[max_len] = 0; confirmed = attempt == 0; }
        else {
            k.header = header; k.sub = sub; k.guide = guide; k.initial = initial;
            k.max_len = max_len; k.min_len = min_len; k.password = rd32(arg + KC_PASSWORD) == 1;
            confirmed = kbd_run_dialog(&k);
        }
        if (!confirmed) break;
        uint64_t bytes = kbd_store(string, k.text, utf8);
        int verdict = 0;
        if (callback && check_buf) {                       /* [sdk] 0x389ed8..0x389ef8 */
            uint64_t message_size = 0, saved_cap = rd64(string + 8), saved[12];
            for (int i = 0; i < 12; i++) saved[i] = c->x[19 + i];
            wr64(string + 8, bytes);
            c->x[0] = check_buf; c->x[1] = P(&message_size); c->x[2] = string;
            call_guest(c, callback);
            verdict = (int)(uint32_t)c->x[0];
            for (int i = 0; i < 12; i++) c->x[19 + i] = saved[i];
            wr64(string + 8, saved_cap);
            if (verdict) {
                wchar_t message[1024];
                if (utf8) { MultiByteToWideChar(CP_UTF8, 0, (const char *)(uintptr_t)check_buf, -1, message, 1023); message[1023] = 0; }
                else copy_w(message, check_buf, 1000);
                trace_printf("     nn::swkbd text check -> %d \"%ls\"\n", verdict, message);
                if (mode != 0) break;                      /* a scripted answer that is refused counts as cancelled */
                if (verdict == 2 && MessageBoxW(NULL, message, L"Software keyboard", MB_YESNO | MB_ICONQUESTION | MB_TOPMOST) == IDYES) verdict = 0;
                else if (verdict != 2) MessageBoxW(NULL, message, L"Software keyboard", MB_OK | MB_ICONWARNING | MB_TOPMOST);
                if (verdict) { wcsncpy(initial, k.text, SWKBD_MAX_CHARS); initial[SWKBD_MAX_CHARS] = 0; continue; }
            }
        }
        result = 0;
        break;
    }
    if (result) trace_printf("     nn::swkbd::ShowKeyboard -> cancelled\n");
    else trace_printf("     nn::swkbd::ShowKeyboard -> \"%ls\"\n", k.text);
    InterlockedExchange(&busy, 0);
    RET(result);
}

/* ================================================================ nn::nfp */
#define NFP_RESULT(desc) ((uint32_t)(115 | ((desc) << 9)))
#define NFP_WRONG_STATE NFP_RESULT(73)
#define NFP_INVALID_ARGUMENT NFP_RESULT(65)
#define NFP_TAG_REMOVED NFP_RESULT(97)
#define NFP_SETTINGS_CLOSED 0x1a073u
enum { NFP_INITIALIZED, NFP_SEARCHING, NFP_FOUND, NFP_REMOVED, NFP_MOUNTED, NFP_UNAVAILABLE };
/* The reader. Its calls come from the game's NFC thread, the amiibo from the amiibo menu on the game's main thread. */
static SRWLOCK g_nfp_lock = SRWLOCK_INIT;
static int g_nfp_init, g_nfp_state = NFP_UNAVAILABLE;
static uint64_t g_nfp_ev_on, g_nfp_ev_off;                 /* the game's activate / deactivate SystemEventType, 0 = not attached */
static struct { int held, read; uint8_t id[8], uid[7]; } g_tag;   /* the amiibo held to the reader; read = the game has its model information */
static volatile LONG g_nfp_reads;                          /* amiibo the game has read (smo_amiibo.c shows it) */
static void nfp_lock(void) { AcquireSRWLockExclusive(&g_nfp_lock); }
static void nfp_unlock(void) { ReleaseSRWLockExclusive(&g_nfp_lock); }
static void nfp_tag_arrives(void) {                       /* lock held, state SearchingForTag, an amiibo held */
    g_nfp_state = NFP_FOUND;
    if (g_nfp_ev_off) os_clear_event(g_nfp_ev_off);
    if (g_nfp_ev_on) os_signal_event(g_nfp_ev_on);
    printf("nfp: amiibo %02x%02x%02x%02x %02x%02x%02x%02x found by the reader\n", g_tag.id[0], g_tag.id[1], g_tag.id[2], g_tag.id[3], g_tag.id[4], g_tag.id[5], g_tag.id[6], g_tag.id[7]);
}
static void nfp_tag_leaves(void) {                        /* lock held, state TagFound or TagMounted */
    g_nfp_state = NFP_REMOVED;
    if (g_nfp_ev_on) os_clear_event(g_nfp_ev_on);
    if (g_nfp_ev_off) os_signal_event(g_nfp_ev_off);
    if (g_tag.read) { g_tag.held = 0; printf("nfp: amiibo taken away from the reader after the game read it\n"); }
}
/* smo_amiibo.c: hold an amiibo (its 8-byte identification block) to the reader, in place of the one there; NULL takes it away. */
void nfp_place_amiibo(const uint8_t *id) {
    nfp_lock();
    if (g_nfp_state == NFP_FOUND || g_nfp_state == NFP_MOUNTED) nfp_tag_leaves();        /* the one there is taken away first */
    memset(&g_tag, 0, sizeof g_tag);
    if (id) {
        uint32_t h = 2166136261u;                          /* a serial number of its own per kind of amiibo: 0x04 (NXP) + 6 bytes */
        g_tag.held = 1;
        memcpy(g_tag.id, id, 8);
        g_tag.uid[0] = 0x04;
        for (int i = 1; i < 7; i++) { for (int k = 0; k < 8; k++) h = (h ^ id[k]) * 16777619u; h = (h ^ (uint32_t)i) * 16777619u; g_tag.uid[i] = (uint8_t)(h >> 13); }
        printf("nfp: amiibo %02x%02x%02x%02x %02x%02x%02x%02x held to the reader (reader %s)\n", id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7],
               g_nfp_state == NFP_SEARCHING ? "searching" : g_nfp_init ? "not searching: found at the next search" : "not initialised");
        if (g_nfp_state == NFP_SEARCHING) nfp_tag_arrives();
    }
    nfp_unlock();
}
/* smo_amiibo.c: is an amiibo held that the game has not read yet; how many the game has read so far */
int nfp_amiibo_waiting(void) { nfp_lock(); int w = g_tag.held && !g_tag.read; nfp_unlock(); return w; }
long nfp_amiibo_reads(void) { return g_nfp_reads; }

static void nfp_Initialize(Ctx *c) { (void)c; nfp_lock(); g_nfp_init = 1; g_nfp_state = NFP_INITIALIZED; nfp_unlock(); }
static void nfp_Finalize(Ctx *c) {
    (void)c;
    nfp_lock();
    if (g_nfp_state == NFP_FOUND || g_nfp_state == NFP_MOUNTED) nfp_tag_leaves();
    g_nfp_init = 0; g_nfp_state = NFP_UNAVAILABLE;
    nfp_unlock();
}
static void nfp_GetState(Ctx *c) { RET(g_nfp_init ? 1 : 0); }
static void nfp_GetDeviceState(Ctx *c) { RET(g_nfp_state); }
static void nfp_ListDevices(Ctx *c) {                      /* (DeviceHandle *out, int *outCount, int max) */
    if (A1) wr32(A1, 0);
    if (!g_nfp_init) { RET(NFP_WRONG_STATE); return; }
    if (!A0 || (int)A2 < 1) { RET(NFP_INVALID_ARGUMENT); return; }
    wr64(A0, 0);                                           /* the reader of npad 0 */
    if (A1) wr32(A1, 1);
    RET(0);
}
static void nfp_GetNpadId(Ctx *c) { if (!g_nfp_init) { RET(NFP_WRONG_STATE); return; } wr32(A0, 0); RET(0); }   /* (uint32_t *out, handle) */
static void nfp_attach(Ctx *c, uint64_t *slot) {           /* (SystemEventType *out, handle) */
    nfp_lock();
    os_init_event(A0, 1);
    *slot = A0;
    if (slot == &g_nfp_ev_on ? g_nfp_state == NFP_FOUND || g_nfp_state == NFP_MOUNTED : g_nfp_state == NFP_REMOVED) os_signal_event(A0);
    RET(g_nfp_init ? 0 : NFP_WRONG_STATE);
    nfp_unlock();
}
static void nfp_AttachEvent(Ctx *c) { nfp_attach(c, &g_nfp_ev_on); }                  /* AttachActivateEvent */
static void nfp_AttachDeactivateEvent(Ctx *c) { nfp_attach(c, &g_nfp_ev_off); }
static void nfp_AttachAvailabilityChangeEvent(Ctx *c) { os_init_event(A0, 0); RET(0); }
static void nfp_StartDetection(Ctx *c) {
    nfp_lock();
    if (!g_nfp_init || (g_nfp_state != NFP_INITIALIZED && g_nfp_state != NFP_REMOVED)) RET(NFP_WRONG_STATE);
    else {
        g_nfp_state = NFP_SEARCHING;
        if (g_tag.held && !g_tag.read) nfp_tag_arrives();
        RET(0);
    }
    nfp_unlock();
}
static void nfp_StopDetection(Ctx *c) {
    nfp_lock();
    if (g_nfp_state == NFP_FOUND || g_nfp_state == NFP_MOUNTED) nfp_tag_leaves();
    if (g_nfp_state == NFP_INITIALIZED) RET(0);
    else if (g_nfp_state == NFP_SEARCHING || g_nfp_state == NFP_REMOVED) { g_nfp_state = NFP_INITIALIZED; RET(0); }
    else RET(NFP_WRONG_STATE);
    nfp_unlock();
}
static void nfp_Mount(Ctx *c) {                            /* (handle, ModelType, MountTarget) */
    nfp_lock();
    if (A1 != 0) RET(NFP_INVALID_ARGUMENT);                /* ModelType 0 = amiibo, the only one */
    else if (g_nfp_state != NFP_FOUND) RET(g_nfp_state == NFP_REMOVED ? NFP_TAG_REMOVED : NFP_WRONG_STATE);
    else { g_nfp_state = NFP_MOUNTED; RET(0); }
    nfp_unlock();
}
static void nfp_Unmount(Ctx *c) {
    nfp_lock();
    if (g_nfp_state != NFP_MOUNTED) RET(g_nfp_state == NFP_REMOVED ? NFP_TAG_REMOVED : NFP_WRONG_STATE);
    else {
        g_nfp_state = NFP_FOUND;
        if (g_tag.read) nfp_tag_leaves();                  /* one touch: the amiibo is taken away once the game has read it */
        RET(0);
    }
    nfp_unlock();
}
static void nfp_GetTagInfo(Ctx *c) {                       /* (TagInfo *out, handle) */
    nfp_lock();
    if (g_nfp_state != NFP_FOUND && g_nfp_state != NFP_MOUNTED) RET(g_nfp_state == NFP_REMOVED ? NFP_TAG_REMOVED : NFP_WRONG_STATE);
    else {
        uint8_t *t = (uint8_t *)(uintptr_t)A0;
        memset(t, 0, 0x58);
        memcpy(t, g_tag.uid, 7);
        t[0xa] = 7;
        wr32(A0 + 0x20, 1);                                /* NfcProtocol TypeA */
        wr32(A0 + 0x24, 2);                                /* TagType Type2 */
        RET(0);
    }
    nfp_unlock();
}
static void nfp_GetModelInfo(Ctx *c) {                     /* (ModelInfo *out, handle) */
    nfp_lock();
    if (g_nfp_state != NFP_MOUNTED) RET(g_nfp_state == NFP_REMOVED ? NFP_TAG_REMOVED : NFP_WRONG_STATE);
    else {
        uint8_t *m = (uint8_t *)(uintptr_t)A0;
        memset(m, 0, 0x40);
        m[0] = g_tag.id[0]; m[1] = g_tag.id[1]; m[2] = g_tag.id[2]; m[3] = g_tag.id[3];
        m[4] = g_tag.id[5]; m[5] = g_tag.id[4];            /* the model number, big-endian on the tag */
        m[6] = g_tag.id[6];
        if (!g_tag.read) { g_tag.read = 1; InterlockedIncrement(&g_nfp_reads); printf("nfp: the game read the amiibo's model information\n"); }
        RET(0);
    }
    nfp_unlock();
}
static void nfp_needs_tag(Ctx *c) { RET(NFP_WRONG_STATE); }                 /* every call that needs a found or mounted tag */
static void nfp_GetApplicationArea(Ctx *c) { if (A1) wr64(A1, 0); RET(NFP_WRONG_STATE); }   /* (void *out, size_t *outSize, handle, size) */
/* StartNicknameAndOwnerSettings (2 forms, [sdk] 0x308970 / 0x308af0), StartGameDataEraser ([sdk] 0x308c50), StartRestorer
 * ([sdk] 0x308d90).  The SDK checks NOTHING before the applet (no device state, no tag presence): it fills the 0x1a8-byte
 * StartParamForAmiiboSettings (mode byte @1: 0 / 1 / 2, flags @2, the caller's device handle @4, TagInfo @0xc, RegisterInfo
 * @0x64) and runs library applet 0xb with StartLibraryAppletEasy ([sdk] 0x3087f0).  The Result is 0x1a073 when the applet
 * could not run or when the first byte of its 0x188-byte answer is 0, and only otherwise are the outputs (device handle from
 * answer+4, the "registered" flag = bit 2, RegisterInfo) written ([sdk] 0x308844..0x308858, 0x308a84..0x308ab8).
 * The applet's answer is the system's: [suyu] am/frontend/applet_cabinet.cpp DisplayCompleted -> Cancel() when the reader's
 * state is neither TagFound nor TagMounted: answer byte 0 (CabinetResult::Cancel).  There is no amiibo settings applet here
 * (and SMO does not import these four), so every call takes that path: 0x1a073 and no output written. */
static void nfp_settings_applet(Ctx *c) {
    trace_printf("     nn::nfp amiibo settings applet -> closed without a change (0x1a073)\n");
    RET(NFP_SETTINGS_CLOSED);
}

/* ================================================================ registration */
#define NFP(n) "_ZN2nn3nfp" n
/* These replace the placeholders nnmisc.c used to hold for the same names (removed there). */
static const ImportDef applet_imports[] = {
    {"_ZN2nn3web19ShowOfflineHtmlPageEPNS0_26OfflineHtmlPageReturnValueERKNS0_22ShowOfflineHtmlPageArgE", web_ShowOfflineHtmlPage},
    {"_ZN2nn3web13ShowLobbyPageEPNS0_20LobbyPageReturnValueERKNS0_16ShowLobbyPageArgE", web_ShowLobbyPage},
    {"_ZN2nn3web11ShowWebPageEPNS0_18WebPageReturnValueERKNS0_14ShowWebPageArgE", web_ShowWebPage},
    {"_ZN2nn3web16ShowYouTubeVideoEPNS0_18WebPageReturnValueERKNS0_19ShowYouTubeVideoArgE", web_ShowYouTubeVideo},
    {"_ZN2nn2ec24ShowShopAddOnContentListENS_13ApplicationIdE", ec_ShowShop},
    {"_ZN2nn2ec24ShowShopAddOnContentListENS_13ApplicationIdERKNS_7account10UserHandleE", ec_ShowShop},
    {"_ZN2nn2ec22ShowShopProductDetailsENS0_5NsUidERKNS_7account10UserHandleE", ec_ShowShop},
    {"_ZN2nn2ec19ShowShopProductListEPKNS0_5NsUidEiPKcRKNS_7account10UserHandleE", ec_ShowShop},
    {"_ZN2nn2oe24GetExpectedVolumeBalanceEPfS1_", oe_GetExpectedVolumeBalance},
    {"_ZN2nn2oe24SetExpectedVolumeBalanceEff", oe_SetExpectedVolumeBalance},
    {"_ZN2nn5swkbd12ShowKeyboardEPNS0_6StringERKNS0_15ShowKeyboardArgE", swkbd_ShowKeyboard},
    {NFP("10InitializeEv"), nfp_Initialize}, {NFP("8FinalizeEv"), nfp_Finalize}, {NFP("8GetStateEv"), nfp_GetState},
    {NFP("14GetDeviceStateERKNS0_12DeviceHandleE"), nfp_GetDeviceState}, {NFP("11ListDevicesEPNS0_12DeviceHandleEPii"), nfp_ListDevices},
    {NFP("9GetNpadIdEPjRKNS0_12DeviceHandleE"), nfp_GetNpadId},
    {NFP("29AttachAvailabilityChangeEventEPNS_2os15SystemEventTypeE"), nfp_AttachAvailabilityChangeEvent},
    {NFP("19AttachActivateEventEPNS_2os15SystemEventTypeERKNS0_12DeviceHandleE"), nfp_AttachEvent},
    {NFP("21AttachDeactivateEventEPNS_2os15SystemEventTypeERKNS0_12DeviceHandleE"), nfp_AttachDeactivateEvent},
    {NFP("14StartDetectionERKNS0_12DeviceHandleE"), nfp_StartDetection}, {NFP("13StopDetectionERKNS0_12DeviceHandleE"), nfp_StopDetection},
    {NFP("5MountERKNS0_12DeviceHandleENS0_9ModelTypeENS0_11MountTargetE"), nfp_Mount},
    {NFP("5MountERKNS0_12DeviceHandleENS0_9ModelTypeE"), nfp_needs_tag}, {NFP("7UnmountERKNS0_12DeviceHandleE"), nfp_Unmount},
    {NFP("19OpenApplicationAreaERKNS0_12DeviceHandleEj"), nfp_needs_tag}, {NFP("5FlushERKNS0_12DeviceHandleE"), nfp_needs_tag},
    {NFP("7RestoreERKNS0_12DeviceHandleE"), nfp_needs_tag}, {NFP("10GetTagInfoEPNS0_7TagInfoERKNS0_12DeviceHandleE"), nfp_GetTagInfo},
    {NFP("15GetRegisterInfoEPNS0_12RegisterInfoERKNS0_12DeviceHandleE"), nfp_needs_tag},
    {NFP("13GetCommonInfoEPNS0_10CommonInfoERKNS0_12DeviceHandleE"), nfp_needs_tag},
    {NFP("12GetModelInfoEPNS0_9ModelInfoERKNS0_12DeviceHandleE"), nfp_GetModelInfo},
    {NFP("18GetApplicationAreaEPvPmRKNS0_12DeviceHandleEm"), nfp_GetApplicationArea},
    {NFP("18SetApplicationAreaERKNS0_12DeviceHandleEPKvm"), nfp_needs_tag},
    {NFP("21CreateApplicationAreaERKNS0_12DeviceHandleERKNS0_25ApplicationAreaCreateInfoE"), nfp_needs_tag},
    {NFP("23RecreateApplicationAreaERKNS0_12DeviceHandleERKNS0_25ApplicationAreaCreateInfoE"), nfp_needs_tag},
    {NFP("29StartNicknameAndOwnerSettingsEPNS0_12DeviceHandleEPbPNS0_12RegisterInfoERKNS0_24AmiiboSettingsStartParamERKNS0_7TagInfoE"), nfp_settings_applet},
    {NFP("29StartNicknameAndOwnerSettingsEPNS0_12DeviceHandleEPbPNS0_12RegisterInfoERKNS0_24AmiiboSettingsStartParamERKNS0_7TagInfoERKS4_"), nfp_settings_applet},
    {NFP("13StartRestorerEPNS0_12DeviceHandleERKNS0_24AmiiboSettingsStartParamERKNS0_7TagInfoE"), nfp_settings_applet},
    {NFP("19StartGameDataEraserEPNS0_12DeviceHandleERKNS0_24AmiiboSettingsStartParamERKNS0_7TagInfoE"), nfp_settings_applet},
};

void register_nnapplet_imports(void) {
    int n = (int)(sizeof applet_imports / sizeof applet_imports[0]);
    for (int i = 0; i < n; i++)
        if (!import_data(applet_imports[i].name)) printf("nnapplet: \"%s\" is not an import of this game version (a typo, or a name another version imports)\n", applet_imports[i].name);
    register_imports(applet_imports, n);
}

/* ================================================================ self-test */
static ImportFn h(const char *name) {
    ImportFn f = import_lookup(name);
    if (!f) { printf("  no handler registered for %s\n", name); report_and_exit(3); }
    return f;
}
static int guard_ok(const uint8_t *p, size_t from, size_t to, uint8_t v) { for (size_t i = from; i < to; i++) if (p[i] != v) return 0; return 1; }
static volatile int g_kbd_test_initial_seen;
static DWORD WINAPI kbd_test_driver(LPVOID arg) {
    (void)arg;
    for (int round = 0; round < 2; round++) {
        HWND w = NULL;
        for (int i = 0; i < 500 && !w; i++) { Sleep(10); w = FindWindowW(NULL, L"Software keyboard (smo-recomp)"); }
        if (!w) return 1;
        Sleep(100);
        if (round == 0) {
            wchar_t now[64] = L"";
            GetDlgItemTextW(w, IDC_EDIT, now, 63);
            g_kbd_test_initial_seen = wcscmp(now, L"Mario") == 0;
            static const wchar_t typed[] = {'Z', 0x30bc, 0};
            SetDlgItemTextW(w, IDC_EDIT, typed);
            PostMessageW(w, WM_COMMAND, IDOK, 0);
        } else PostMessageW(w, WM_COMMAND, IDCANCEL, 0);
        for (int i = 0; i < 500 && IsWindow(w); i++) Sleep(10);
    }
    return 0;
}
/* An amiibo held to the reader, in the order the game's NFC thread asks ([game] 0x95efa8): found at StartDetection with the
 * activate event signalled; Mount; TagInfo / ModelInfo; Unmount takes it away (deactivate event); searching again finds
 * nothing. Run by both self-tests (SMO_SDK_SELFTEST in nnapplet_selftest, SMO_IMPORT_SELFTEST in smo_selftest.c). */
int nfp_amiibo_selftest(Ctx *c) {
    static const uint8_t peach_wedding[8] = {0x00, 0x02, 0x00, 0x00, 0x03, 0x72, 0x09, 0x02};
    uint64_t handle = 0, on[8], off[8];
    uint8_t tag[0x60], model[0x48];
    int ok;
    memset(tag, 0xee, sizeof tag); memset(model, 0xee, sizeof model);
    long reads = nfp_amiibo_reads();
    nfp_Initialize(c);
    c->x[0] = P(on); c->x[1] = P(&handle); nfp_AttachEvent(c); ok = c->x[0] == 0;
    c->x[0] = P(off); c->x[1] = P(&handle); nfp_AttachDeactivateEvent(c); ok = ok && c->x[0] == 0;
    nfp_place_amiibo(peach_wedding);
    ok = ok && nfp_amiibo_waiting();
    c->x[0] = P(&handle); nfp_GetDeviceState(c); ok = ok && c->x[0] == NFP_INITIALIZED;        /* not searching: nothing found yet */
    c->x[0] = P(&handle); nfp_StartDetection(c); ok = ok && c->x[0] == 0;
    c->x[0] = P(&handle); nfp_GetDeviceState(c); ok = ok && c->x[0] == NFP_FOUND;
    c->x[0] = P(on); h("_ZN2nn2os18TryWaitSystemEventEPNS0_15SystemEventTypeE")(c); ok = ok && (c->x[0] & 1);
    c->x[0] = P(on); h("_ZN2nn2os18TryWaitSystemEventEPNS0_15SystemEventTypeE")(c); ok = ok && !(c->x[0] & 1);   /* AutoClear */
    c->x[0] = P(&handle); c->x[1] = 1; c->x[2] = 1; nfp_Mount(c); ok = ok && c->x[0] == NFP_INVALID_ARGUMENT;        /* ModelType 1 */
    c->x[0] = P(&handle); c->x[1] = 0; c->x[2] = 1; nfp_Mount(c); ok = ok && c->x[0] == 0;
    c->x[0] = P(&handle); nfp_GetDeviceState(c); ok = ok && c->x[0] == NFP_MOUNTED;
    c->x[0] = P(tag); c->x[1] = P(&handle); nfp_GetTagInfo(c);
    ok = ok && c->x[0] == 0 && tag[0] == 0x04 && tag[0xa] == 7 && rd32(P(tag) + 0x20) == 1 && rd32(P(tag) + 0x24) == 2 && guard_ok(tag, 0x58, sizeof tag, 0xee);
    c->x[0] = P(model); c->x[1] = P(&handle); nfp_GetModelInfo(c);
    ok = ok && c->x[0] == 0 && model[0] == 0x00 && model[1] == 0x02 && model[2] == 0x00 && rd16(P(model) + 4) == 882 && model[6] == 0x09 && guard_ok(model, 0x40, sizeof model, 0xee);
    ok = ok && nfp_amiibo_reads() == reads + 1 && !nfp_amiibo_waiting();
    c->x[0] = P(&handle); nfp_Unmount(c); ok = ok && c->x[0] == 0;
    c->x[0] = P(&handle); nfp_GetDeviceState(c); ok = ok && c->x[0] == NFP_REMOVED;
    c->x[0] = P(off); h("_ZN2nn2os18TryWaitSystemEventEPNS0_15SystemEventTypeE")(c); ok = ok && (c->x[0] & 1);
    c->x[0] = P(tag); c->x[1] = P(&handle); nfp_GetTagInfo(c); ok = ok && c->x[0] == NFP_TAG_REMOVED;
    c->x[0] = P(&handle); nfp_StartDetection(c); ok = ok && c->x[0] == 0;
    c->x[0] = P(&handle); nfp_GetDeviceState(c); ok = ok && c->x[0] == NFP_SEARCHING;             /* read once: not found again */
    c->x[0] = P(&handle); nfp_StopDetection(c); ok = ok && c->x[0] == 0;
    nfp_place_amiibo(NULL);
    nfp_Finalize(c);
    printf("  nfp: an amiibo held to the reader: found at StartDetection (activate event, AutoClear), Mount, TagInfo (type A, type 2, 7-byte serial), ModelInfo (0x000200 #882 series 9), taken away at Unmount (deactivate event, TagRemoved): %s\n", ok ? "ok" : "FAILED");
    return !ok;
}

int nnapplet_selftest(Ctx *c) {
    int bad = 0, ok;
    uint8_t *buf = malloc(0x8000), *buf2 = malloc(0x8000);
    /* --- nn::web: argument objects (lifted) stay inside 0x2000 bytes; return values have the documented shape */
    memset(buf, 0xee, 0x8000);
    c->x[0] = P(buf); c->x[1] = P("help/index.html"); h("_ZN2nn3web22ShowOfflineHtmlPageArgC1EPKc")(c);
    c->x[0] = P(buf); c->x[1] = 1; h("_ZN2nn3web22ShowOfflineHtmlPageArg16SetFooterEnabledEb")(c);
    c->x[0] = P(buf); c->x[1] = 1; h("_ZN2nn3web22ShowOfflineHtmlPageArg17SetPointerEnabledEb")(c);
    c->x[0] = P(buf); c->x[1] = 12; c->x[2] = 4; h("_ZN2nn3web22ShowOfflineHtmlPageArg17SetKeyRepeatFrameEii")(c);
    const char *doc = web_arg_string(P(buf), 6);
    ok = guard_ok(buf, 0x2000, 0x8000, 0xee) && doc && strstr(doc, "help/index.html") != NULL;
    printf("  web: ShowOfflineHtmlPageArg (lifted): document path TLV \"%s\", %d TLVs, nothing written past 0x2000: %s\n", doc ? doc : "(none)", rd16(P(buf)), ok ? "ok" : "FAILED");
    bad += !ok;
    memset(buf2, 0xee, 0x8000);
    c->x[0] = P(buf2); h("_ZN2nn3web26OfflineHtmlPageReturnValueC1Ev")(c);
    ok = rd64(P(buf2)) == 0x14 && guard_ok(buf2, 0x1010, 0x8000, 0xee);
    c->x[0] = P(buf2); c->x[1] = P(buf); web_ShowOfflineHtmlPage(c);
    ok = ok && c->x[0] == 0 && rd32(P(buf2)) == 0 && strcmp((char *)buf2 + 8, "http://localhost/") == 0 && rd64(P(buf2) + 0x1008) == 17 && guard_ok(buf2, 0x1010, 0x8000, 0xee);
    printf("  web: OfflineHtmlPageReturnValue: constructor 0x14, after the page: exit reason 0, last URL set, 0x1010 bytes: %s\n", ok ? "ok" : "FAILED");
    bad += !ok;
    memset(buf, 0xee, 0x8000);
    c->x[0] = P(buf); c->x[1] = P("https://example.invalid/"); h("_ZN2nn3web14ShowWebPageArgC1EPKc")(c);
    c->x[0] = P(buf); c->x[1] = P("https://example.invalid/cb"); h("_ZN2nn3web14ShowWebPageArg14SetCallbackUrlEPKc")(c);
    ok = guard_ok(buf, 0x2000, 0x8000, 0xee);
    memset(buf2, 0xee, 0x8000);
    c->x[0] = P(buf2); h("_ZN2nn3web18WebPageReturnValueC1Ev")(c);
    c->x[0] = P(buf2); h("_ZNK2nn3web18WebPageReturnValue16GetWebExitReasonEv")(c);
    ok = ok && c->x[0] == 0x14;                            /* no TLV yet */
    c->x[0] = P(buf2); c->x[1] = P(buf); web_ShowWebPage(c);
    ok = ok && c->x[0] == 0 && guard_ok(buf2, 0x2000, 0x8000, 0xee);
    c->x[0] = P(buf2); h("_ZNK2nn3web18WebPageReturnValue16GetWebExitReasonEv")(c);      /* the SDK's own getters read what was written */
    ok = ok && c->x[0] == 0;
    c->x[0] = P(buf2); h("_ZNK2nn3web18WebPageReturnValue10GetLastUrlEv")(c);
    ok = ok && c->x[0] && strcmp((const char *)(uintptr_t)c->x[0], "http://localhost/") == 0;
    c->x[0] = P(buf2); h("_ZN2nn3web35IsMediaPlayerAutoClosedByCompletionERKNS0_18WebPageReturnValueE")(c);
    ok = ok && (c->x[0] & 1) == 0;
    printf("  web: WebPageReturnValue TLVs read back by the SDK's getters (exit reason 0, last URL, not auto-closed): %s\n", ok ? "ok" : "FAILED");
    bad += !ok;
    /* --- nn::swkbd: config (lifted) is 0x4d0 bytes; a scripted answer goes through ShowKeyboard */
    memset(buf, 0xee, 0x8000);
    c->x[0] = P(buf); c->x[1] = 0; h("_ZN2nn5swkbd10MakePresetEPNS0_14KeyboardConfigENS0_6PresetE")(c);
    static const uint16_t guide[] = {'N', 'a', 'm', 'e', 0}, initial_text[] = {'M', 'a', 'r', 'i', 'o', 0};
    c->x[0] = P(buf); c->x[1] = P(guide); h("_ZN2nn5swkbd12SetGuideTextEPNS0_14KeyboardConfigEPKDs")(c);
    c->x[0] = P(buf); c->x[1] = P(guide); h("_ZN2nn5swkbd13SetHeaderTextEPNS0_14KeyboardConfigEPKDs")(c);
    c->x[0] = P(buf); c->x[1] = P(guide); h("_ZN2nn5swkbd10SetSubTextEPNS0_14KeyboardConfigEPKDs")(c);
    ok = guard_ok(buf, 0x4d0, 0x8000, 0xee) && rd16(P(buf) + KC_GUIDE) == 'N' && rd16(P(buf) + KC_HEADER) == 'N';
    printf("  swkbd: MakePreset + Set*Text (lifted) stay inside 0x4d0 bytes, texts at 0x24 / 0x1a8: %s\n", ok ? "ok" : "FAILED");
    bad += !ok;
    c->x[0] = 0; h("_ZN2nn5swkbd25GetRequiredWorkBufferSizeEb")(c);
    uint64_t work_size = c->x[0];
    h("_ZN2nn5swkbd27GetRequiredStringBufferSizeEv")(c);
    uint64_t string_size = c->x[0];
    uint8_t *work = calloc(1, work_size + 16), *text = malloc(string_size + 16);
    memset(buf + 0x4d0, 0, 0x30);                          /* the rest of ShowKeyboardArg */
    wr64(P(buf) + ARG_WORKBUF, P(work)); wr64(P(buf) + ARG_WORKBUF + 8, work_size);
    c->x[0] = P(buf); c->x[1] = P(initial_text); h("_ZN2nn5swkbd14SetInitialTextEPNS0_15ShowKeyboardArgEPKDs")(c);
    ok = rd32(P(buf) + KC_INITIAL_LENGTH) == 5 && rd16(P(work) + rd32(P(buf) + KC_INITIAL_OFFSET)) == 'M';
    uint64_t string[2] = {P(text), string_size};
    memset(text, 0xee, string_size + 16);
    _putenv("SMO_SWKBD=text:Kirby");
    c->x[0] = P(string); c->x[1] = P(buf); swkbd_ShowKeyboard(c);
    ok = ok && c->x[0] == 0 && rd16(P(text)) == 'K' && rd16(P(text) + 8) == 'y' && rd16(P(text) + 10) == 0 && guard_ok(text, string_size, string_size + 16, 0xee);
    _putenv("SMO_SWKBD=cancel");
    c->x[0] = P(string); c->x[1] = P(buf); swkbd_ShowKeyboard(c);
    ok = ok && c->x[0] == SWKBD_RESULT_CANCELED;
    /* the real dialog: a helper thread types into it and presses OK, then a second run is cancelled */
    _putenv("SMO_SWKBD=dialog");
    HANDLE helper = CreateThread(NULL, 0, kbd_test_driver, NULL, 0, NULL);
    memset(text, 0xee, string_size + 16);
    c->x[0] = P(string); c->x[1] = P(buf); swkbd_ShowKeyboard(c);
    int dialog_ok = c->x[0] == 0 && rd16(P(text)) == 'Z' && rd16(P(text) + 2) == 0x30bc && rd16(P(text) + 4) == 0 && g_kbd_test_initial_seen;
    c->x[0] = P(string); c->x[1] = P(buf); swkbd_ShowKeyboard(c);
    dialog_ok = dialog_ok && c->x[0] == SWKBD_RESULT_CANCELED;
    WaitForSingleObject(helper, 5000);
    CloseHandle(helper);
    printf("  swkbd: Win32 dialog shows the initial text \"Mario\", returns typed text \"Z\" + U+30BC on OK and 0x29f on Cancel: %s\n", dialog_ok ? "ok" : "FAILED");
    bad += !dialog_ok;
    _putenv("SMO_SWKBD=");
    printf("  swkbd: sizes work 0x%llx string 0x%llx; SetInitialText in the work buffer; scripted answer \"Kirby\" (UTF-16) and cancel (0x29f): %s\n",
           (unsigned long long)work_size, (unsigned long long)string_size, ok ? "ok" : "FAILED");
    bad += !ok;
    free(work); free(text);
    /* --- nn::nfp state machine */
    uint64_t handle = 0xeeeeeeeeeeeeeeeeull, ev[8];
    int count = -1;
    nfp_Finalize(c);
    nfp_GetState(c); ok = c->x[0] == 0;
    c->x[0] = P(&handle); nfp_GetDeviceState(c); ok = ok && c->x[0] == 5;
    nfp_Initialize(c);
    nfp_GetState(c); ok = ok && c->x[0] == 1;
    c->x[0] = P(&handle); c->x[1] = P(&count); c->x[2] = 1; nfp_ListDevices(c); ok = ok && c->x[0] == 0 && count == 1 && handle == 0;
    c->x[0] = P(&handle); nfp_GetDeviceState(c); ok = ok && c->x[0] == 0;
    c->x[0] = P(&handle); nfp_StartDetection(c); ok = ok && c->x[0] == 0;
    c->x[0] = P(&handle); nfp_GetDeviceState(c); ok = ok && c->x[0] == 1;
    c->x[0] = P(&handle); c->x[1] = 0; nfp_needs_tag(c); ok = ok && c->x[0] == NFP_WRONG_STATE;
    c->x[0] = P(&handle); nfp_StopDetection(c); ok = ok && c->x[0] == 0;
    c->x[0] = P(ev); c->x[1] = P(&handle); nfp_AttachEvent(c); ok = ok && c->x[0] == 0;
    nfp_Finalize(c);
    printf("  nfp: Initialize / ListDevices (1 reader) / StartDetection / StopDetection state machine, tag calls refused: %s\n", ok ? "ok" : "FAILED");
    bad += !ok;
    bad += nfp_amiibo_selftest(c);
    {   /* amiibo settings applets: 0x1a073 and no output written ([sdk] 0x308844..0x308858, 0x308a84) */
        static const char *const names[4] = {
            NFP("29StartNicknameAndOwnerSettingsEPNS0_12DeviceHandleEPbPNS0_12RegisterInfoERKNS0_24AmiiboSettingsStartParamERKNS0_7TagInfoE"),
            NFP("29StartNicknameAndOwnerSettingsEPNS0_12DeviceHandleEPbPNS0_12RegisterInfoERKNS0_24AmiiboSettingsStartParamERKNS0_7TagInfoERKS4_"),
            NFP("13StartRestorerEPNS0_12DeviceHandleERKNS0_24AmiiboSettingsStartParamERKNS0_7TagInfoE"),
            NFP("19StartGameDataEraserEPNS0_12DeviceHandleERKNS0_24AmiiboSettingsStartParamERKNS0_7TagInfoE")};
        uint8_t out_handle[8], out_flag[1], out_reg[0x100], in_a[0x100], in_b[0x100], in_c[0x100];
        ok = 1;
        for (int i = 0; i < 4; i++) {
            ImportFn f = import_lookup(names[i]);
            memset(out_handle, 0x5a, sizeof out_handle); memset(out_flag, 0x5a, sizeof out_flag); memset(out_reg, 0x5a, sizeof out_reg);
            memset(in_a, 0, sizeof in_a); memset(in_b, 0, sizeof in_b); memset(in_c, 0, sizeof in_c);
            c->x[0] = P(out_handle);
            if (i < 2) { c->x[1] = P(out_flag); c->x[2] = P(out_reg); c->x[3] = P(in_a); c->x[4] = P(in_b); c->x[5] = P(in_c); }
            else { c->x[1] = P(in_a); c->x[2] = P(in_b); }
            if (!f) { ok = 0; continue; }
            f(c);
            ok = ok && c->x[0] == 0x1a073 && out_flag[0] == 0x5a;
            for (int k = 0; k < 8; k++) ok = ok && out_handle[k] == 0x5a;
            for (int k = 0; k < 0x100; k++) ok = ok && out_reg[k] == 0x5a;
        }
        printf("  nfp: the 4 amiibo settings applets with no amiibo: 0x1a073, device handle / flag / RegisterInfo untouched: %s\n", ok ? "ok" : "FAILED");
        bad += !ok;
    }
    {   /* nn::ec shop pages: void, nothing written through any argument ([sdk] 0x18cad0, 0x1907a0, 0x1933a0) */
        uint8_t arg[0x40], uid[0x20];
        memset(arg, 0x5a, sizeof arg); memset(uid, 0x5a, sizeof uid);
        c->x[0] = P(arg); c->x[1] = 2; c->x[2] = P(arg + 0x20); c->x[3] = P(uid); ec_ShowShop(c);
        ok = 1;
        for (int k = 0; k < 0x40; k++) ok = ok && arg[k] == 0x5a;
        for (int k = 0; k < 0x20; k++) ok = ok && uid[k] == 0x5a;
        printf("  ec: ShowShop* returns and writes nothing: %s\n", ok ? "ok" : "FAILED");
        bad += !ok;
    }
    {   /* nn::mii::AppendMii cancelled: 0xe7e, *outIndex untouched ([sdk] 0x300bc4, 0x301040) */
        ImportFn f = import_lookup("_ZN2nn3mii9AppendMiiEPi");
        int32_t index = 0x5a5a5a5a;
        c->x[0] = P(&index);
        if (f) f(c);
        ok = f && c->x[0] == 0xe7e && index == 0x5a5a5a5a;
        printf("  mii: AppendMii answered as cancelled: 0xe7e, index untouched: %s\n", ok ? "ok" : "FAILED");
        bad += !ok;
    }
    /* --- nn::oe */
    float a = -1, b = -1, a0 = g_volume_main, b0 = g_volume_library;
    c->v[0].f[0] = 0.5f; c->v[1].f[0] = 3.0f; oe_SetExpectedVolumeBalance(c);
    c->x[0] = P(&a); c->x[1] = P(&b); oe_GetExpectedVolumeBalance(c);
    ok = a == 0.5f && b == 1.0f && a0 == 0.25f && b0 == 1.0f;
    c->v[0].f[0] = a0; c->v[1].f[0] = b0; oe_SetExpectedVolumeBalance(c);
    printf("  oe: volume balance initial 0.25 / 1.0, set / get (clamped to 1.0): %s\n", ok ? "ok" : "FAILED");
    bad += !ok;
    free(buf); free(buf2);
    return bad;
}
