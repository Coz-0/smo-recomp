/* smo_guide_mac.m - the game's Help (its Action Guide, the console's offline web applet) inside the game window on macOS:
 * what smo_guide.c does with WebView2 on Windows, with a WKWebView over the SDL window's content view.
 *   - the main thread (SDL's, which pumps the window's events: boot.c) owns the web view. The first opening makes it and adds
 *     two scripts to every document before the page's own: window.chrome.webview over WebKit's message handler (the host
 *     nnapplet.c's window.nx looks for) and then nnapplet.c's script itself; later openings load and show the page again.
 *     Closing only hides it, so the next opening is quick. Nothing is written next to the page.
 *   - the page closes itself through window.nx.endApplet (the script posts "endApplet" and calls window.close()), or by
 *     going to a callback address (http://localhost/...: the Action Guide's "change mode" item), which ends it with that
 *     address, as on the console and on Windows.
 *   - keys go to the page (it is the first responder while open): the script turns Escape / Backspace into B, Q / E / Page
 *     Up / Page Down into L / R; F11 still switches fullscreen. Pads are read on the waiting thread (hid_menu_pad_buttons)
 *     and handed to the page through window.__smo_pad, exactly as on Windows.
 *   - the page gets the largest 16:9 area of the window, centred, zoomed so that the manual's 1280-pixel layout fills it;
 *     it follows the window's size.
 *   - while it is open the game is held (display_menu_hold); buttons still held when it closes are kept from the game
 *     until let go (hid_menu_closed).
 * Test aid: SMO_GUIDE_SHOT=<file.png>, as on Windows: the page 1.5 s after it has loaded and 0.8 s after each pad action
 * (<file>_2.png, ...; at most 8). */
#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
#include <SDL3/SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#if !__has_feature(objc_arc)
#error "smo_guide_mac.m needs ARC (-fobjc-arc)"
#endif

void display_menu_hold(int on);                     /* display.c */
int display_closing(void);
void display_toggle_fullscreen(void);
void *display_cocoa_window(void);
uint64_t hid_menu_pad_buttons(void);                 /* nnhid.c */
void hid_menu_closed(void);

enum { ST_IDLE, ST_OPENING, ST_OPEN, ST_CLOSED, ST_FAILED };
enum { GUIDE_OPEN = 1, GUIDE_PAD, GUIDE_CLOSE };
#define BTN(n) (1ull << (n))
enum { B_A = 0, B_B = 1, B_L = 6, B_R = 7, B_ZL = 8, B_ZR = 9, B_LEFT = 12, B_UP = 13, B_RIGHT = 14, B_DOWN = 15,
       B_SL_LEFT = 16, B_SL_UP = 17, B_SL_RIGHT = 18, B_SL_DOWN = 19 };

static volatile int g_state;                          /* ST_*: written on the main thread, waited for by the guest thread */
static volatile int g_called_back;
static char g_callback[4096], g_url[4096];
static char *g_script;
static int g_shot_n;

@interface SmoGuide : NSObject <WKScriptMessageHandler, WKNavigationDelegate, WKUIDelegate>
@property(strong) WKWebView *view;
@property(strong) id keyMonitor;
@end
static SmoGuide *g_guide;

static void set_state(int s) { __atomic_store_n(&g_state, s, __ATOMIC_SEQ_CST); }
static int state(void) { return __atomic_load_n(&g_state, __ATOMIC_SEQ_CST); }

static void fit(void) {
    NSWindow *win = (__bridge NSWindow *)display_cocoa_window();
    if (!g_guide.view || !win) return;
    NSRect b = win.contentView.bounds;
    CGFloat w = b.size.width, h = b.size.height, pw = w, ph = w * 9 / 16;
    if (w <= 0 || h <= 0) return;
    if (ph > h) { ph = h; pw = h * 16 / 9; }
    g_guide.view.frame = NSMakeRect((w - pw) / 2, (h - ph) / 2, pw, ph);
    g_guide.view.pageZoom = pw / 1280.0;
}
static void shot_later(double seconds);
static void hide_page(int st) {
    NSWindow *win = (__bridge NSWindow *)display_cocoa_window();
    g_guide.view.hidden = YES;
    [g_guide.view loadHTMLString:@"" baseURL:nil];      /* stops the page's timers and its videos */
    if (win) [win makeFirstResponder:win.contentView];
    if (g_guide.keyMonitor) { [NSEvent removeMonitor:g_guide.keyMonitor]; g_guide.keyMonitor = nil; }
    set_state(st);
}

@implementation SmoGuide
- (void)userContentController:(WKUserContentController *)uc didReceiveScriptMessage:(WKScriptMessage *)m {
    if ([m.body isKindOfClass:[NSString class]] && [m.body isEqualToString:@"endApplet"] && state() == ST_OPEN) hide_page(ST_CLOSED);
}
- (void)webViewDidClose:(WKWebView *)v { if (state() == ST_OPEN) hide_page(ST_CLOSED); }
/* a callback address (http://localhost/...) ends the page as the console's browser does: the navigation is not made */
- (void)webView:(WKWebView *)v decidePolicyForNavigationAction:(WKNavigationAction *)a decisionHandler:(void (^)(WKNavigationActionPolicy))done {
    NSString *u = a.request.URL.absoluteString;
    if (state() == ST_OPEN && !g_called_back && [u.lowercaseString hasPrefix:@"http://localhost/"]) {
        done(WKNavigationActionPolicyCancel);
        snprintf(g_callback, sizeof g_callback, "%s", u.UTF8String);
        __atomic_store_n(&g_called_back, 1, __ATOMIC_SEQ_CST);
        dispatch_async(dispatch_get_main_queue(), ^{ if (state() == ST_OPEN) hide_page(ST_CLOSED); });
        return;
    }
    done(WKNavigationActionPolicyAllow);
}
- (void)webView:(WKWebView *)v didFinishNavigation:(WKNavigation *)n {
    if (state() != ST_OPEN || [v.URL.scheme isEqualToString:@"about"] || !v.URL) return;       /* the blank page after closing */
    printf("guide: the page has loaded\n");
    fflush(stdout);
    if (getenv("SMO_GUIDE_SHOT")) shot_later(1.5);
}
- (void)webView:(WKWebView *)v didFailProvisionalNavigation:(WKNavigation *)n withError:(NSError *)e {
    if (state() == ST_OPEN && !g_called_back && e.code != NSURLErrorCancelled) { printf("guide: the page did NOT load: %s\n", e.localizedDescription.UTF8String); fflush(stdout); }
}
@end

static void shot(void) {
    const char *base = getenv("SMO_GUIDE_SHOT"), *dot = strrchr(base, '.');
    if (!g_guide.view || state() != ST_OPEN || g_shot_n >= 8) return;
    char path[1024];
    if (++g_shot_n == 1) snprintf(path, sizeof path, "%s", base);
    else snprintf(path, sizeof path, "%.*s_%d%s", dot ? (int)(dot - base) : (int)strlen(base), base, g_shot_n, dot ? dot : "");
    NSString *p = [NSString stringWithUTF8String:path];
    [g_guide.view takeSnapshotWithConfiguration:nil completionHandler:^(NSImage *img, NSError *err) {
        NSData *png = nil;
        if (img) {
            CGImageRef cg = [img CGImageForProposedRect:NULL context:nil hints:nil];
            if (cg) png = [[[NSBitmapImageRep alloc] initWithCGImage:cg] representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
        }
        BOOL ok = png && [png writeToFile:p atomically:YES];
        printf("guide: SMO_GUIDE_SHOT: the page %s %s\n", ok ? "written to" : "NOT written to", p.UTF8String);
        fflush(stdout);
    }];
}
static void shot_later(double seconds) { dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(seconds * 1e9)), dispatch_get_main_queue(), ^{ shot(); }); }

static int make_view(NSWindow *win) {
    WKWebViewConfiguration *cfg = [[WKWebViewConfiguration alloc] init];
    WKUserContentController *uc = cfg.userContentController;
    g_guide = [[SmoGuide alloc] init];
    [uc addScriptMessageHandler:g_guide name:@"smo"];
    /* window.chrome.webview, the host nnapplet.c's script talks to (WebView2's on Windows) */
    [uc addUserScript:[[WKUserScript alloc] initWithSource:@"window.chrome = window.chrome || {}; window.chrome.webview = { postMessage: function (m) { window.webkit.messageHandlers.smo.postMessage(String(m)); } };"
                                             injectionTime:WKUserScriptInjectionTimeAtDocumentStart forMainFrameOnly:NO]];
    [uc addUserScript:[[WKUserScript alloc] initWithSource:[NSString stringWithUTF8String:g_script] injectionTime:WKUserScriptInjectionTimeAtDocumentStart forMainFrameOnly:NO]];
    [cfg.preferences setValue:@YES forKey:@"allowFileAccessFromFileURLs"];
    WKWebView *v = [[WKWebView alloc] initWithFrame:win.contentView.bounds configuration:cfg];
    if (!v) return 0;
    v.navigationDelegate = g_guide; v.UIDelegate = g_guide;
    v.allowsMagnification = NO; v.allowsBackForwardNavigationGestures = NO;
    [v setValue:@NO forKey:@"drawsBackground"];        /* no white flash before the page's own background */
    v.hidden = YES;
    [win.contentView addSubview:v positioned:NSWindowAbove relativeTo:nil];
    g_guide.view = v;
    [[NSNotificationCenter defaultCenter] addObserverForName:NSViewFrameDidChangeNotification object:win.contentView queue:nil
                                                  usingBlock:^(NSNotification *n) { if (state() == ST_OPEN) fit(); }];
    win.contentView.postsFrameChangedNotifications = YES;
    printf("guide: WKWebView ready in the game window\n");
    return 1;
}

/* the requests of guide_show, on the main thread */
static void on_request(void *arg) {
    uintptr_t what = (uintptr_t)arg & 0xff, pad = (uintptr_t)arg >> 8;
    @autoreleasepool {
        NSWindow *win = (__bridge NSWindow *)display_cocoa_window();
        if (what == GUIDE_OPEN) {
            if (!win || (!g_guide && !make_view(win))) { printf("guide: no window for the page: it goes to the web browser instead\n"); set_state(ST_FAILED); return; }
            NSURL *url = [NSURL URLWithString:[NSString stringWithUTF8String:g_url]];
            NSString *path = url.path;
            NSRange r = [path rangeOfString:@"/html-document/"];
            NSURL *root = r.location != NSNotFound ? [NSURL fileURLWithPath:[path substringToIndex:r.location] isDirectory:YES] : url.URLByDeletingLastPathComponent;
            if (!url || !url.isFileURL) { printf("guide: not a file address: %s\n", g_url); set_state(ST_FAILED); return; }
            fit();
            g_guide.view.hidden = NO;
            set_state(ST_OPEN);
            [g_guide.view loadFileURL:url allowingReadAccessToURL:root];
            [win makeFirstResponder:g_guide.view];
            g_guide.keyMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown handler:^NSEvent *(NSEvent *e) {
                if (e.keyCode == 103 && state() == ST_OPEN) { display_toggle_fullscreen(); return nil; }    /* F11, also while the page has the keys */
                return e;
            }];
        } else if (what == GUIDE_PAD && g_guide.view && state() == ST_OPEN) {
            static const char *const k_pad[] = {"A", "B", "L", "R", "U", "D", "Lf", "Rt"};
            if (pad >= sizeof k_pad / sizeof k_pad[0]) return;
            [g_guide.view evaluateJavaScript:[NSString stringWithFormat:@"window.__smo_pad && window.__smo_pad('%s')", k_pad[pad]] completionHandler:nil];
            if (getenv("SMO_GUIDE_SHOT")) shot_later(0.8);
        } else if (what == GUIDE_CLOSE && state() == ST_OPEN) {
            hide_page(ST_CLOSED);
        }
    }
}
static void post(int what, int pad) { SDL_RunOnMainThread(on_request, (void *)(uintptr_t)(what | pad << 8), false); }

void guide_on_size(void) {}                          /* the page follows the window by itself (NSViewFrameDidChangeNotification) */

/* nnapplet.c web_open_offline, on the guest thread that called ShowOfflineHtmlPage: as smo_guide.c on Windows. */
int guide_show(const char *url, const char *script, char *callback, size_t cap) {
    if (!display_cocoa_window()) return 0;
    if (state() == ST_FAILED) return 0;              /* the page could not be shown earlier in this run */
    if (!g_script) g_script = strdup(script);         /* added once: it stays for every later document */
    snprintf(g_url, sizeof g_url, "%s", url);
    g_callback[0] = 0;
    __atomic_store_n(&g_called_back, 0, __ATOMIC_SEQ_CST);
    set_state(ST_OPENING);
    post(GUIDE_OPEN, 0);
    for (int t = 0; state() == ST_OPENING && t < 30000 && !display_closing(); t += 10) usleep(10000);
    if (display_closing()) { post(GUIDE_CLOSE, 0); set_state(ST_IDLE); return 1; }    /* no browser while the game is ending */
    if (state() != ST_OPEN) {
        if (state() == ST_OPENING) printf("guide: the web view did not start within 30 s: the page goes to the web browser instead\n");
        set_state(ST_FAILED);
        return 0;
    }
    printf("guide: open in the game window (%s)\n", url);
    fflush(stdout);
    display_menu_hold(1);
    uint64_t prev = hid_menu_pad_buttons();          /* what is held when it opens counts only once let go */
    int held = 0, last_dir = -1;
    while (state() == ST_OPEN) {
        if (display_closing()) { post(GUIDE_CLOSE, 0); break; }
        uint64_t b = hid_menu_pad_buttons(), press = b & ~prev;
        prev = b;
        if (press & BTN(B_A)) post(GUIDE_PAD, 0);
        if (press & BTN(B_B)) post(GUIDE_PAD, 1);
        if (press & (BTN(B_L) | BTN(B_ZL))) post(GUIDE_PAD, 2);
        if (press & (BTN(B_R) | BTN(B_ZR))) post(GUIDE_PAD, 3);
        int dir = b & (BTN(B_UP) | BTN(B_SL_UP)) ? 4 : b & (BTN(B_DOWN) | BTN(B_SL_DOWN)) ? 5 : b & (BTN(B_LEFT) | BTN(B_SL_LEFT)) ? 6
                : b & (BTN(B_RIGHT) | BTN(B_SL_RIGHT)) ? 7 : -1;
        if (dir != last_dir) held = 0;               /* a direction: at once, then as the pause menu repeats */
        if (dir >= 0 && (held == 0 || (held >= 30 && (held - 30) % 5 == 0))) post(GUIDE_PAD, dir);
        if (dir >= 0) held++;
        last_dir = dir;
        usleep(16000);
    }
    hid_menu_closed();
    display_menu_hold(0);
    set_state(ST_IDLE);
    if (__atomic_load_n(&g_called_back, __ATOMIC_SEQ_CST)) {
        snprintf(callback, cap, "%s", g_callback);
        printf("guide: the page went to %s: closed with that callback address, back to the game\n", g_callback);
        fflush(stdout);
        return 2;
    }
    printf("guide: closed, back to the game\n");
    fflush(stdout);
    return 1;
}
