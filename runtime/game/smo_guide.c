/* smo_guide.c - the console's offline web applet (the game's Help, its Action Guide) INSIDE the game window.
 *
 * On the console nn::web::ShowOfflineHtmlPage starts the browser applet, which shows the game's HTML manual over the game,
 * and the calling thread waits until the player closes it. Here the same page (the manual in the dump, read in place) is
 * shown by Microsoft Edge WebView2 in the game window, covering it, while the calling guest thread waits:
 *   - the window thread (display.c, an STA with a message loop) owns the WebView2 objects. The first opening creates the
 *     environment and the controller (both asynchronously, on that thread) and adds nnapplet.c's script (the console
 *     browser's window.nx) to every document before the page's own scripts run; later openings navigate and show the page
 *     again. Closing only hides it, so the next opening is quick. Nothing is written next to the page.
 *   - the page closes itself through window.nx.endApplet (the script posts "endApplet" and calls window.close()), or by
 *     going to a callback address (http://localhost/...: the Action Guide's "change mode" item): the console's browser then
 *     ends with exit reason CallbackURL and that address, which nnapplet.c hands to the game.
 *   - keys go to the page itself (it has the keyboard focus): the script turns Escape / Backspace into B, Q / E / Page Up /
 *     Page Down into L / R; the page has its own arrow keys. Pads are read by the runtime on the waiting thread
 *     (hid_menu_pad_buttons: the game's own pad layer and mappings) and handed to the page as footer presses (A B L R) and
 *     arrow keys through window.__smo_pad; the script then leaves the browser's Gamepad API alone.
 *   - while it is open the game is held as for the amiibo menu (display_menu_hold: the refreshes are not counted, so the
 *     game does not run ahead afterwards, and the freeze watchdog stays quiet); buttons still held when it closes are
 *     kept from the game until let go (hid_menu_closed).
 *   - WebView2Loader.dll (from the WebView2 SDK, copied next to the exe by the build) is loaded on first use, and the
 *     WebView2 Runtime is part of Windows 11. When either is missing, guide_show returns 0 and nnapplet.c opens the page
 *     in the PC's web browser instead (said in the log).
 * Test aid: SMO_GUIDE_SHOT=<file.png> writes the page as shown 1.5 s after it has loaded, and 0.8 s after each pad action as
 * <file>_2.png, _3.png, ... (at most 8 pictures a run; the game's own screenshots do not see the page: the renderer does not
 * draw it). */
#define COBJMACROS
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <shlwapi.h>
#else
#include "win32_posix.h"
#endif
#ifndef _WIN32
/* macOS: no in-window page yet (a WKWebView over the game window is step 6 of docs/macos-port.md). guide_show answers 0,
 * so nnapplet.c opens the page in the web browser, as it does on Windows without WebView2. */
int guide_show(const char *url, const char *script, char *callback, size_t cap) {
    (void)url; (void)script; (void)callback; (void)cap;
    printf("guide: the page is not shown in the game window on macOS yet: it goes to the web browser instead\n");
    return 0;
}
void guide_on_size(void) {}
#else
#include "WebView2.h"

void display_menu_hold(int on);                     /* display.c */
int display_closing(void);
void display_toggle_fullscreen(void);
void *display_hwnd(void);
uint64_t hid_menu_pad_buttons(void);                 /* nnhid.c */
void hid_menu_closed(void);

#define WM_SMO_GUIDE (WM_APP + 0x47)
enum { GUIDE_OPEN = 1, GUIDE_PAD, GUIDE_CLOSE };
enum { ST_IDLE, ST_OPENING, ST_OPEN, ST_CLOSED, ST_FAILED };
/* button bits of hid_menu_pad_buttons (nnhid.c: nn::hid NpadButton order; the left stick as the BTN_SL_* bits) */
#define BTN(n) (1ull << (n))
enum { B_A = 0, B_B = 1, B_L = 6, B_R = 7, B_ZL = 8, B_ZR = 9, B_LEFT = 12, B_UP = 13, B_RIGHT = 14, B_DOWN = 15,
       B_SL_LEFT = 16, B_SL_UP = 17, B_SL_RIGHT = 18, B_SL_DOWN = 19 };

static HWND g_parent;
static ICoreWebView2Environment *g_env;
static ICoreWebView2Controller *g_ctl;
static ICoreWebView2 *g_wv;
static volatile LONG g_state;                         /* ST_*: written by the window thread, waited for by the guest thread */
static volatile LONG g_called_back;                   /* the page went to a callback address (g_callback) */
static int g_creating, g_shot_n;
static char g_shot_path[MAX_PATH], g_callback[4096];
static wchar_t g_url[4096], *g_script;
static HMODULE g_loader;
typedef HRESULT (STDAPICALLTYPE *CreateEnvFn)(PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions *, ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *);

/* The interface ids of the handlers below ([doc] WebView2.h of the WebView2 SDK; the header only declares them). */
static const IID k_iid_unknown = {0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const IID k_iid_env_done = {0x4e8a3389, 0xc9d8, 0x4bd2, {0xb6, 0xb5, 0x12, 0x4f, 0xee, 0x6c, 0xc1, 0x4d}};
static const IID k_iid_ctl_done = {0x6c4819f3, 0xc9b7, 0x4260, {0x81, 0x27, 0xc9, 0xf5, 0xbd, 0xe7, 0xf6, 0x8c}};
static const IID k_iid_message = {0x57213f19, 0x00e6, 0x49fa, {0x8e, 0x07, 0x89, 0x8e, 0xa0, 0x1e, 0xcb, 0xd2}};
static const IID k_iid_close = {0x5c19e9e0, 0x092f, 0x486b, {0xaf, 0xfa, 0xca, 0x82, 0x31, 0x91, 0x30, 0x39}};
static const IID k_iid_navigated = {0xd33a35bf, 0x1c49, 0x4f98, {0x93, 0xab, 0x00, 0x6e, 0x05, 0x33, 0xfe, 0x1c}};
static const IID k_iid_key = {0xb29c7e28, 0xfa79, 0x41a8, {0x8e, 0x44, 0x65, 0x81, 0x1c, 0x76, 0xdc, 0xb2}};
static const IID k_iid_shot = {0x697e05e9, 0x3d8f, 0x45fa, {0x96, 0xf4, 0x8f, 0xfe, 0x1e, 0xde, 0xda, 0xf5}};
static const IID k_iid_script = {0x49511172, 0xcc67, 0x4bca, {0x99, 0x23, 0x13, 0x71, 0x12, 0xf4, 0xc4, 0xcc}};
static const IID k_iid_starting = {0x9adbe429, 0xf36d, 0x432b, {0x9d, 0xdc, 0xf8, 0x88, 0x1f, 0xbd, 0x76, 0xe3}};
static const IID k_iid_added = {0xb99369f3, 0x9b11, 0x47b5, {0xbc, 0x6f, 0x8e, 0x78, 0x95, 0xfc, 0xea, 0x17}};

/* COM objects for the callbacks: one static instance of each, no reference counting (they live as long as the process). */
#define HANDLER(T, iid, invoke_args)                                                                                     \
    static HRESULT STDMETHODCALLTYPE T##_qi(T *This, REFIID riid, void **pp) {                                         \
        if (IsEqualIID(riid, &k_iid_unknown) || IsEqualIID(riid, &iid)) { *pp = This; return S_OK; }                      \
        *pp = NULL; return E_NOINTERFACE;                                                                                \
    }                                                                                                                    \
    static ULONG STDMETHODCALLTYPE T##_ref(T *This) { (void)This; return 1; }                                           \
    static HRESULT STDMETHODCALLTYPE T##_invoke invoke_args;
HANDLER(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler, k_iid_env_done,
        (ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *This, HRESULT hr, ICoreWebView2Environment *env))
HANDLER(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler, k_iid_ctl_done,
        (ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *This, HRESULT hr, ICoreWebView2Controller *ctl))
HANDLER(ICoreWebView2WebMessageReceivedEventHandler, k_iid_message,
        (ICoreWebView2WebMessageReceivedEventHandler *This, ICoreWebView2 *wv, ICoreWebView2WebMessageReceivedEventArgs *args))
HANDLER(ICoreWebView2WindowCloseRequestedEventHandler, k_iid_close,
        (ICoreWebView2WindowCloseRequestedEventHandler *This, ICoreWebView2 *wv, IUnknown *args))
HANDLER(ICoreWebView2NavigationCompletedEventHandler, k_iid_navigated,
        (ICoreWebView2NavigationCompletedEventHandler *This, ICoreWebView2 *wv, ICoreWebView2NavigationCompletedEventArgs *args))
HANDLER(ICoreWebView2AcceleratorKeyPressedEventHandler, k_iid_key,
        (ICoreWebView2AcceleratorKeyPressedEventHandler *This, ICoreWebView2Controller *ctl, ICoreWebView2AcceleratorKeyPressedEventArgs *args))
HANDLER(ICoreWebView2CapturePreviewCompletedHandler, k_iid_shot, (ICoreWebView2CapturePreviewCompletedHandler *This, HRESULT hr))
HANDLER(ICoreWebView2ExecuteScriptCompletedHandler, k_iid_script, (ICoreWebView2ExecuteScriptCompletedHandler *This, HRESULT hr, LPCWSTR result))
HANDLER(ICoreWebView2NavigationStartingEventHandler, k_iid_starting,
        (ICoreWebView2NavigationStartingEventHandler *This, ICoreWebView2 *wv, ICoreWebView2NavigationStartingEventArgs *args))
HANDLER(ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler, k_iid_added,
        (ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler *This, HRESULT hr, LPCWSTR id))
#define VTBL(T) static T##Vtbl T##_vt = {T##_qi, T##_ref, T##_ref, T##_invoke}; static T T##_obj = {&T##_vt};
VTBL(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler)
VTBL(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler)
VTBL(ICoreWebView2WebMessageReceivedEventHandler)
VTBL(ICoreWebView2WindowCloseRequestedEventHandler)
VTBL(ICoreWebView2NavigationCompletedEventHandler)
VTBL(ICoreWebView2AcceleratorKeyPressedEventHandler)
VTBL(ICoreWebView2CapturePreviewCompletedHandler)
VTBL(ICoreWebView2ExecuteScriptCompletedHandler)
VTBL(ICoreWebView2NavigationStartingEventHandler)
VTBL(ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler)

static void fail_open(const char *what, HRESULT hr) {
    printf("guide: %s failed (0x%08lx): the page goes to the web browser instead\n", what, (unsigned long)hr);
    fflush(stdout);
    g_creating = 0;
    InterlockedExchange(&g_state, ST_FAILED);
}
/* The manual is laid out for the console's 1280 x 720 screen (its style sheet has fixed sizes), so the page gets the largest
 * 16:9 area of the window, centred, and is zoomed to fill it: in a 2560 x 1440 window it would otherwise fill the top-left
 * quarter. */
static void fit(void) {
    RECT rc;
    if (!g_ctl || !GetClientRect(g_parent, &rc) || rc.right <= 0 || rc.bottom <= 0) return;
    int w = rc.right, h = rc.bottom, pw = w, ph = w * 9 / 16;
    if (ph > h) { ph = h; pw = h * 16 / 9; }
    RECT page = {(w - pw) / 2, (h - ph) / 2, (w - pw) / 2 + pw, (h - ph) / 2 + ph};
    ICoreWebView2Controller_put_Bounds(g_ctl, page);
    ICoreWebView2Controller_put_ZoomFactor(g_ctl, pw / 1280.0);
}
static void show_page(void) {
    fit();
    ICoreWebView2Controller_put_IsVisible(g_ctl, TRUE);
    HRESULT hr = ICoreWebView2_Navigate(g_wv, g_url);
    if (FAILED(hr)) { fail_open("ICoreWebView2::Navigate", hr); ICoreWebView2Controller_put_IsVisible(g_ctl, FALSE); return; }
    ICoreWebView2Controller_MoveFocus(g_ctl, COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    InterlockedExchange(&g_state, ST_OPEN);
}
static void hide_page(int state) {
    if (g_ctl) ICoreWebView2Controller_put_IsVisible(g_ctl, FALSE);
    if (g_wv) ICoreWebView2_Navigate(g_wv, L"about:blank");             /* stops the page's timers and its videos */
    SetFocus(g_parent);
    InterlockedExchange(&g_state, state);
}

static HRESULT STDMETHODCALLTYPE ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler_invoke(
        ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *This, HRESULT hr, ICoreWebView2Environment *env) {
    (void)This;
    if (FAILED(hr) || !env) { fail_open("creating the WebView2 environment (is the WebView2 Runtime installed?)", hr); return S_OK; }
    g_env = env;
    ICoreWebView2Environment_AddRef(env);
    hr = ICoreWebView2Environment_CreateCoreWebView2Controller(env, g_parent, &ICoreWebView2CreateCoreWebView2ControllerCompletedHandler_obj);
    if (FAILED(hr)) fail_open("ICoreWebView2Environment::CreateCoreWebView2Controller", hr);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE ICoreWebView2CreateCoreWebView2ControllerCompletedHandler_invoke(
        ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *This, HRESULT hr, ICoreWebView2Controller *ctl) {
    (void)This;
    if (FAILED(hr) || !ctl) { fail_open("creating the WebView2 controller", hr); return S_OK; }
    g_ctl = ctl;
    ICoreWebView2Controller_AddRef(ctl);
    if (FAILED(ICoreWebView2Controller_get_CoreWebView2(ctl, &g_wv)) || !g_wv) { fail_open("ICoreWebView2Controller::get_CoreWebView2", E_FAIL); return S_OK; }
    ICoreWebView2Settings *s = NULL;
    if (SUCCEEDED(ICoreWebView2_get_Settings(g_wv, &s)) && s) {        /* an applet, not a browser: no menus, zoom, dev tools */
        ICoreWebView2Settings_put_AreDefaultContextMenusEnabled(s, FALSE);
        ICoreWebView2Settings_put_AreDevToolsEnabled(s, FALSE);
        ICoreWebView2Settings_put_IsZoomControlEnabled(s, FALSE);
        ICoreWebView2Settings_put_IsStatusBarEnabled(s, FALSE);
        ICoreWebView2Settings_Release(s);
    }
    EventRegistrationToken tok;
    ICoreWebView2_add_WebMessageReceived(g_wv, &ICoreWebView2WebMessageReceivedEventHandler_obj, &tok);
    ICoreWebView2_add_WindowCloseRequested(g_wv, &ICoreWebView2WindowCloseRequestedEventHandler_obj, &tok);
    ICoreWebView2_add_NavigationStarting(g_wv, &ICoreWebView2NavigationStartingEventHandler_obj, &tok);
    ICoreWebView2_add_NavigationCompleted(g_wv, &ICoreWebView2NavigationCompletedEventHandler_obj, &tok);
    ICoreWebView2Controller_add_AcceleratorKeyPressed(ctl, &ICoreWebView2AcceleratorKeyPressedEventHandler_obj, &tok);
    /* window.nx before the page's own scripts, in every document from now on; the page is shown once it is in place */
    hr = ICoreWebView2_AddScriptToExecuteOnDocumentCreated(g_wv, g_script, &ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler_obj);
    if (FAILED(hr)) fail_open("ICoreWebView2::AddScriptToExecuteOnDocumentCreated", hr);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler_invoke(
        ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler *This, HRESULT hr, LPCWSTR id) {
    (void)This; (void)id;
    if (FAILED(hr)) { fail_open("adding window.nx to the page (AddScriptToExecuteOnDocumentCreated)", hr); return S_OK; }
    g_creating = 0;
    printf("guide: WebView2 ready in the game window\n");
    show_page();
    return S_OK;
}
/* A callback address (http://localhost/...) ends the page as the console's browser does: the navigation is not made, and
 * guide_show returns the address. */
static HRESULT STDMETHODCALLTYPE ICoreWebView2NavigationStartingEventHandler_invoke(
        ICoreWebView2NavigationStartingEventHandler *This, ICoreWebView2 *wv, ICoreWebView2NavigationStartingEventArgs *args) {
    (void)This; (void)wv;
    LPWSTR uri = NULL;
    if (g_state != ST_OPEN || FAILED(ICoreWebView2NavigationStartingEventArgs_get_Uri(args, &uri)) || !uri) return S_OK;
    if (!_wcsnicmp(uri, L"http://localhost/", 17) && !g_called_back) {
        ICoreWebView2NavigationStartingEventArgs_put_Cancel(args, TRUE);
        WideCharToMultiByte(CP_UTF8, 0, uri, -1, g_callback, (int)sizeof g_callback, NULL, NULL);
        InterlockedExchange(&g_called_back, 1);
        PostMessageW(g_parent, WM_SMO_GUIDE, GUIDE_CLOSE, 0);         /* hide_page outside this event */
    }
    CoTaskMemFree(uri);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE ICoreWebView2WebMessageReceivedEventHandler_invoke(
        ICoreWebView2WebMessageReceivedEventHandler *This, ICoreWebView2 *wv, ICoreWebView2WebMessageReceivedEventArgs *args) {
    (void)This; (void)wv;
    LPWSTR msg = NULL;
    if (SUCCEEDED(ICoreWebView2WebMessageReceivedEventArgs_TryGetWebMessageAsString(args, &msg)) && msg) {
        if (!wcscmp(msg, L"endApplet") && g_state == ST_OPEN) hide_page(ST_CLOSED);
        CoTaskMemFree(msg);
    }
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE ICoreWebView2WindowCloseRequestedEventHandler_invoke(
        ICoreWebView2WindowCloseRequestedEventHandler *This, ICoreWebView2 *wv, IUnknown *args) {
    (void)This; (void)wv; (void)args;
    if (g_state == ST_OPEN) hide_page(ST_CLOSED);
    return S_OK;
}
static IStream *g_shot_stream;
static HRESULT STDMETHODCALLTYPE ICoreWebView2CapturePreviewCompletedHandler_invoke(ICoreWebView2CapturePreviewCompletedHandler *This, HRESULT hr) {
    (void)This;
    if (g_shot_stream) { IStream_Release(g_shot_stream); g_shot_stream = NULL; }     /* closes the file */
    printf("guide: SMO_GUIDE_SHOT: the page %s %s\n", SUCCEEDED(hr) ? "written to" : "NOT written to", g_shot_path);
    fflush(stdout);
    return S_OK;
}
static void CALLBACK shot_timer(HWND h, UINT m, UINT_PTR id, DWORD t) {    /* SMO_GUIDE_SHOT: after loading, after a pad action */
    (void)m; (void)t;
    KillTimer(h, id);
    wchar_t w[MAX_PATH];
    const char *base = getenv("SMO_GUIDE_SHOT"), *dot = strrchr(base, '.');
    if (!g_wv || g_state != ST_OPEN || g_shot_stream || g_shot_n >= 8) return;
    if (++g_shot_n == 1) snprintf(g_shot_path, sizeof g_shot_path, "%s", base);
    else snprintf(g_shot_path, sizeof g_shot_path, "%.*s_%d%s", dot ? (int)(dot - base) : (int)strlen(base), base, g_shot_n, dot ? dot : "");
    MultiByteToWideChar(CP_UTF8, 0, g_shot_path, -1, w, MAX_PATH);
    if (SUCCEEDED(SHCreateStreamOnFileEx(w, STGM_CREATE | STGM_WRITE, FILE_ATTRIBUTE_NORMAL, TRUE, NULL, &g_shot_stream)))
        ICoreWebView2_CapturePreview(g_wv, COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG, g_shot_stream, &ICoreWebView2CapturePreviewCompletedHandler_obj);
}
static HRESULT STDMETHODCALLTYPE ICoreWebView2NavigationCompletedEventHandler_invoke(
        ICoreWebView2NavigationCompletedEventHandler *This, ICoreWebView2 *wv, ICoreWebView2NavigationCompletedEventArgs *args) {
    (void)This; (void)wv;
    BOOL ok = FALSE;
    ICoreWebView2NavigationCompletedEventArgs_get_IsSuccess(args, &ok);
    if (g_state != ST_OPEN) return S_OK;                                 /* about:blank after closing */
    printf("guide: the page %s\n", ok ? "has loaded" : "did NOT load");
    fflush(stdout);
    if (ok && getenv("SMO_GUIDE_SHOT")) SetTimer(g_parent, 0x4755, 1500, shot_timer);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE ICoreWebView2AcceleratorKeyPressedEventHandler_invoke(
        ICoreWebView2AcceleratorKeyPressedEventHandler *This, ICoreWebView2Controller *ctl, ICoreWebView2AcceleratorKeyPressedEventArgs *args) {
    (void)This; (void)ctl;
    COREWEBVIEW2_KEY_EVENT_KIND kind;
    UINT key = 0;
    ICoreWebView2AcceleratorKeyPressedEventArgs_get_KeyEventKind(args, &kind);
    ICoreWebView2AcceleratorKeyPressedEventArgs_get_VirtualKey(args, &key);
    if (key == VK_F11 && kind == COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN) {    /* the game window's own key, also while the page has the focus */
        ICoreWebView2AcceleratorKeyPressedEventArgs_put_Handled(args, TRUE);
        display_toggle_fullscreen();
    }
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE ICoreWebView2ExecuteScriptCompletedHandler_invoke(ICoreWebView2ExecuteScriptCompletedHandler *This, HRESULT hr, LPCWSTR result) {
    (void)This; (void)hr; (void)result;
    return S_OK;
}

/* display.c's window procedure, on the window thread: the requests of guide_show, and the window's size. */
void guide_on_message(HWND hwnd, WPARAM what, LPARAM arg) {
    g_parent = hwnd;
    if (what == GUIDE_OPEN) {
        if (g_wv) { show_page(); return; }
        if (g_creating) return;
        CreateEnvFn create = g_loader ? (CreateEnvFn)(void *)GetProcAddress(g_loader, "CreateCoreWebView2EnvironmentWithOptions") : NULL;
        if (!create) { fail_open("WebView2Loader.dll (CreateCoreWebView2EnvironmentWithOptions)", E_NOINTERFACE); return; }
        g_creating = 1;
        HRESULT hr = create(NULL, NULL, NULL, &ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler_obj);
        if (FAILED(hr)) fail_open("CreateCoreWebView2EnvironmentWithOptions", hr);
    } else if (what == GUIDE_PAD && g_wv && g_state == ST_OPEN) {
        static const wchar_t *const k_pad[] = {L"A", L"B", L"L", L"R", L"U", L"D", L"Lf", L"Rt"};
        wchar_t js[96];
        if ((size_t)arg >= sizeof k_pad / sizeof k_pad[0]) return;
        swprintf(js, sizeof js / sizeof js[0], L"window.__smo_pad && window.__smo_pad('%ls')", k_pad[arg]);
        ICoreWebView2_ExecuteScript(g_wv, js, &ICoreWebView2ExecuteScriptCompletedHandler_obj);
        if (getenv("SMO_GUIDE_SHOT")) SetTimer(g_parent, 0x4755, 800, shot_timer);
    } else if (what == GUIDE_CLOSE && g_state == ST_OPEN) {
        hide_page(ST_CLOSED);
    }
}
void guide_on_size(void) { if (g_ctl && g_state == ST_OPEN) fit(); }

/* nnapplet.c web_open_offline, on the guest thread that called ShowOfflineHtmlPage: shows the page (a file URL with its
 * query) in the game window, with script (window.nx) added before the page's own scripts, and returns when it has been
 * closed: 1, or 2 when it went to a callback address (copied to callback). Returns 0 at once when it cannot be shown here. */
int guide_show(const char *url, const char *script, char *callback, size_t cap) {
    HWND hwnd = (HWND)display_hwnd();
    if (!hwnd) return 0;
    if (!g_script) {                                                      /* added once: it stays for every later document */
        int n = MultiByteToWideChar(CP_UTF8, 0, script, -1, NULL, 0);
        if (n <= 0 || !(g_script = malloc((size_t)n * sizeof *g_script))) return 0;
        MultiByteToWideChar(CP_UTF8, 0, script, -1, g_script, n);
    }
    if (!g_loader) {
        wchar_t exe[MAX_PATH], *slash;
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        slash = wcsrchr(exe, L'\\');
        if (slash) wcscpy(slash + 1, L"WebView2Loader.dll");
        g_loader = LoadLibraryW(exe);
        if (!g_loader) { printf("guide: WebView2Loader.dll is not next to the exe (error %lu): the page goes to the web browser instead\n", GetLastError()); return 0; }
    }
    if (g_state == ST_FAILED) return 0;                                   /* WebView2 did not start earlier in this run */
    MultiByteToWideChar(CP_UTF8, 0, url, -1, g_url, (int)(sizeof g_url / sizeof g_url[0]));
    g_callback[0] = 0;
    InterlockedExchange(&g_called_back, 0);
    InterlockedExchange(&g_state, ST_OPENING);
    if (!PostMessageW(hwnd, WM_SMO_GUIDE, GUIDE_OPEN, 0)) { InterlockedExchange(&g_state, ST_IDLE); return 0; }
    for (int t = 0; g_state == ST_OPENING && t < 30000 && !display_closing(); t += 10) Sleep(10);
    if (display_closing()) { PostMessageW(hwnd, WM_SMO_GUIDE, GUIDE_CLOSE, 0); InterlockedExchange(&g_state, ST_IDLE); return 1; }   /* no browser while the game is ending */
    if (g_state != ST_OPEN) {
        if (g_state == ST_OPENING) printf("guide: WebView2 did not start within 30 s: the page goes to the web browser instead\n");
        InterlockedExchange(&g_state, ST_FAILED);
        return 0;
    }
    printf("guide: open in the game window (%s)\n", url);
    fflush(stdout);
    display_menu_hold(1);
    uint64_t prev = hid_menu_pad_buttons();                               /* what is held when it opens counts only once let go */
    int held = 0, last_dir = -1;
    while (g_state == ST_OPEN) {
        if (display_closing()) { PostMessageW(hwnd, WM_SMO_GUIDE, GUIDE_CLOSE, 0); break; }
        uint64_t b = hid_menu_pad_buttons(), press = b & ~prev;
        prev = b;
        if (press & BTN(B_A)) PostMessageW(hwnd, WM_SMO_GUIDE, GUIDE_PAD, 0);
        if (press & BTN(B_B)) PostMessageW(hwnd, WM_SMO_GUIDE, GUIDE_PAD, 1);
        if (press & (BTN(B_L) | BTN(B_ZL))) PostMessageW(hwnd, WM_SMO_GUIDE, GUIDE_PAD, 2);
        if (press & (BTN(B_R) | BTN(B_ZR))) PostMessageW(hwnd, WM_SMO_GUIDE, GUIDE_PAD, 3);
        int dir = b & (BTN(B_UP) | BTN(B_SL_UP)) ? 4 : b & (BTN(B_DOWN) | BTN(B_SL_DOWN)) ? 5 : b & (BTN(B_LEFT) | BTN(B_SL_LEFT)) ? 6
                : b & (BTN(B_RIGHT) | BTN(B_SL_RIGHT)) ? 7 : -1;
        if (dir != last_dir) held = 0;                                    /* a direction: at once, then as the pause menu repeats */
        if (dir >= 0 && (held == 0 || (held >= 30 && (held - 30) % 5 == 0))) PostMessageW(hwnd, WM_SMO_GUIDE, GUIDE_PAD, dir);
        if (dir >= 0) held++;
        last_dir = dir;
        Sleep(16);
    }
    hid_menu_closed();
    display_menu_hold(0);
    InterlockedExchange(&g_state, ST_IDLE);
    if (g_called_back) {
        snprintf(callback, cap, "%s", g_callback);
        printf("guide: the page went to %s: closed with that callback address, back to the game\n", g_callback);
        fflush(stdout);
        return 2;
    }
    printf("guide: closed, back to the game\n");
    fflush(stdout);
    return 1;
}
#endif
