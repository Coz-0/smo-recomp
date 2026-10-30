/* game/smo_guide_mac.m, the Help page in the game window on macOS, without the game: an SDL window as display.c makes it
 * (hidden, as for test copies; SMO_SHOW=1 shows it), a page laid out as the manual is (1280 x 720, a picture in another
 * folder of the manual, window.nx through window.chrome.webview as nnapplet.c's script uses it), a guest thread in
 * guide_show and a pad that a third thread presses. The main thread pumps SDL's events, as boot.c's does. Checks:
 * the page loads and fills the largest 16:9 area of the window, zoomed to its 1280-pixel layout (SMO_GUIDE_SHOT
 * pictures, read back); R reaches the page's footer handler; B ends it through endApplet; a callback address
 * (http://localhost/...) ends it with that address; it follows the window's size; the game is held while it is open.
 * Exit code = failures. */
#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>

static int g_fail, g_pass;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int guide_show(const char *url, const char *script, char *callback, size_t cap);

/* ---- stand-ins for display.c and nnhid.c */
static SDL_Window *g_win;
static volatile int g_hold, g_hold_max, g_closed_calls, g_fullscreen;
static volatile uint64_t g_buttons;
void *display_cocoa_window(void) { return g_win ? SDL_GetPointerProperty(SDL_GetWindowProperties(g_win), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, NULL) : NULL; }
int display_closing(void) { return 0; }
void display_menu_hold(int on) { g_hold = on; if (on) g_hold_max = 1; }
void display_toggle_fullscreen(void) { g_fullscreen++; }
uint64_t hid_menu_pad_buttons(void) { return g_buttons; }
void hid_menu_closed(void) { g_closed_calls++; }

/* ---- what nnapplet.c's script does for the host (window.nx, window.__smo_pad, endApplet through chrome.webview) */
static const char k_nx[] =
    "(function () { var keys = {}; var host = window.chrome && window.chrome.webview;\n"
    "  window.__smo_pad = function (b) { if (keys[b]) keys[b](); else if (b === 'A' && document.getElementById('cb')) document.getElementById('cb').click(); };\n"
    "  window.nx = { footer: { setAssign: function (b, l, fn) { keys[b] = fn; } },\n"
    "                endApplet: function () { if (host) host.postMessage('endApplet'); window.close(); } };\n"
    "})();\n";

static char g_dir[512], g_url[1024], g_shot[600];
static void write_file(const char *path, const void *data, size_t n) { FILE *f = fopen(path, "wb"); fwrite(data, 1, n, f); fclose(f); }
static void make_manual(void) {
    snprintf(g_dir, sizeof g_dir, "%s/smo-guide-test", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    char p[700];
    mkdir(g_dir, 0755);
    snprintf(p, sizeof p, "%s/html-document", g_dir); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/html-document/pics", g_dir); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/html-document/legal", g_dir); mkdir(p, 0755);
    /* a 2x2 PNG in another folder of the manual: the page shows its box red only when the picture loaded */
    NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:2 pixelsHigh:2 bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO
                                                                colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:8 bitsPerPixel:32];
    memset(rep.bitmapData, 255, 16);
    NSData *png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    snprintf(p, sizeof p, "%s/html-document/pics/p.png", g_dir);
    write_file(p, png.bytes, png.length);
    static const char page[] =
        "<!doctype html><html><head><meta charset=\"utf-8\"><style>html,body{margin:0;overflow:hidden;background:#204060}"
        "#box{position:absolute;left:0;top:0;width:1280px;height:720px;background:#808080}</style></head><body>"
        "<div id=box></div><a id=cb href=\"http://localhost/changemode?mode=2\">mode</a><img id=pic style=\"position:absolute;left:0;top:0;width:1px\" src=\"../pics/p.png\">"
        "<script>var box = document.getElementById('box');"
        "var pic = document.getElementById('pic'); function shown() { box.style.background = '#c03020'; }"
        "if (pic.complete && pic.naturalWidth) shown(); else pic.onload = shown;"
        "nx.footer.setAssign('R', '', function () { box.style.background = '#30c040'; });"
        "nx.footer.setAssign('B', '', function () { nx.endApplet(); });</script></body></html>";
    snprintf(p, sizeof p, "%s/html-document/legal/guide.html", g_dir);
    write_file(p, page, sizeof page - 1);
    snprintf(g_url, sizeof g_url, "file://%s/html-document/legal/guide.html?lang=en", g_dir);
    snprintf(g_shot, sizeof g_shot, "%s/shot.png", g_dir);
}

/* the colour of a picture at (fx, fy) of its size; 0 when it cannot be read */
static int pixel(const char *path, double fx, double fy, int *r, int *g, int *b, int *w, int *h) {
    NSBitmapImageRep *rep = (NSBitmapImageRep *)[NSBitmapImageRep imageRepWithContentsOfFile:@(path)];
    if (!rep) return 0;
    *w = (int)rep.pixelsWide; *h = (int)rep.pixelsHigh;
    NSColor *c = [[rep colorAtX:(NSInteger)(fx * (*w - 1)) y:(NSInteger)(fy * (*h - 1))] colorUsingColorSpace:NSColorSpace.deviceRGBColorSpace];
    *r = (int)(c.redComponent * 255 + 0.5); *g = (int)(c.greenComponent * 255 + 0.5); *b = (int)(c.blueComponent * 255 + 0.5);
    return 1;
}
static int near(int r, int g, int b, int R, int G, int B) { return abs(r - R) < 24 && abs(g - G) < 24 && abs(b - B) < 24; }
static int all_corners(const char *path, int R, int G, int B, int *w, int *h) {     /* the box fills the picture: inner corners and centre */
    static const double at[5][2] = {{0.01, 0.01}, {0.99, 0.01}, {0.01, 0.99}, {0.99, 0.99}, {0.5, 0.5}};
    for (int i = 0; i < 5; i++) { int r, g, b; if (!pixel(path, at[i][0], at[i][1], &r, &g, &b, w, h) || !near(r, g, b, R, G, B)) { printf("  %s at (%.2f, %.2f): %d %d %d\n", path, at[i][0], at[i][1], r, g, b); return 0; } }
    return 1;
}

/* ---- the guest thread, and the pad */
static volatile int g_done, g_ret[2];
static char g_cb[512];
static void press(int bit) { g_buttons = 1ull << bit; usleep(100000); g_buttons = 0; }
static void *guest(void *arg) {
    (void)arg;
    g_ret[0] = guide_show(g_url, k_nx, g_cb, sizeof g_cb);           /* R, then B (endApplet) */
    g_ret[1] = guide_show(g_url, k_nx, g_cb, sizeof g_cb);           /* A: the callback link */
    g_done = 1;
    return NULL;
}
static volatile int g_step;                                            /* what the pad thread has done, for the main thread's checks */
static void *pad(void *arg) {
    (void)arg;
    usleep(3200000);                     /* loaded, first picture taken (1.5 s after loading) */
    g_step = 1;                          /* the main thread resizes the window and measures */
    while (g_step == 1) usleep(10000);
    press(7);                            /* R: the page turns green; a picture 0.8 s later */
    usleep(1500000);
    press(1);                            /* B: endApplet */
    usleep(2500000);
    press(0);                            /* the second opening: A follows the callback link */
    return NULL;
}

int main(void) {
    @autoreleasepool {
        setvbuf(stdout, NULL, _IONBF, 0);
        make_manual();
        unlink(g_shot);
        setenv("SMO_GUIDE_SHOT", g_shot, 1);
        if (!SDL_Init(SDL_INIT_VIDEO)) { printf("SDL_Init: %s\n", SDL_GetError()); return 1; }
        int visible = getenv("SMO_SHOW") && atoi(getenv("SMO_SHOW"));
        g_win = SDL_CreateWindow("guide test", 1280, 720, SDL_WINDOW_METAL | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE | (visible ? 0 : SDL_WINDOW_HIDDEN));
        SDL_Metal_CreateView(g_win);
        pthread_t gt, pt;
        pthread_create(&gt, NULL, guest, NULL);
        pthread_create(&pt, NULL, pad, NULL);
        NSWindow *win = (__bridge NSWindow *)display_cocoa_window();
        WKWebView *wv = nil;
        double t0 = SDL_GetTicks();
        while (!g_done && SDL_GetTicks() - t0 < 30000) {
            SDL_PumpEvents();
            if (g_step == 1) {                 /* the page in a window of another shape: 1600 x 700 */
                for (NSView *v in win.contentView.subviews) if ([v isKindOfClass:[WKWebView class]]) wv = (WKWebView *)v;
                NSRect f0 = wv.frame;
                CHECK(wv && fabs(f0.size.width - 1280) < 1 && fabs(f0.size.height - 720) < 1 && fabs(wv.pageZoom - 1.0) < 0.01, "the page covers the 1280 x 720 window at zoom 1 (%.0f x %.0f, %.3f)", f0.size.width, f0.size.height, wv.pageZoom);
                SDL_SetWindowSize(g_win, 1600, 700);
                SDL_SyncWindow(g_win);
                for (int i = 0; i < 20; i++) { SDL_PumpEvents(); usleep(5000); }
                NSRect f = wv.frame, cb = win.contentView.bounds;
                printf("  window %.0f x %.0f: page at (%.1f, %.1f) %.1f x %.1f, zoom %.4f\n", cb.size.width, cb.size.height, f.origin.x, f.origin.y, f.size.width, f.size.height, wv.pageZoom);
                double ph = cb.size.height, pw = ph * 16 / 9;
                CHECK(fabs(f.size.height - ph) < 1 && fabs(f.size.width - pw) < 1 && fabs(f.origin.x - (cb.size.width - pw) / 2) < 1 && fabs(wv.pageZoom - pw / 1280) < 0.002,
                      "in a 1600 x 700 window the page is the largest 16:9 area, centred, zoomed to %.4f", pw / 1280);
                SDL_SetWindowSize(g_win, 1280, 720);
                SDL_SyncWindow(g_win);
                for (int i = 0; i < 20; i++) { SDL_PumpEvents(); usleep(5000); }
                CHECK(g_hold == 1, "the game is held while the page is open");
                g_step = 2;
            }
            usleep(2000);
        }
        pthread_join(gt, NULL); pthread_join(pt, NULL);
        for (int i = 0; i < 50; i++) { SDL_PumpEvents(); usleep(10000); }      /* the last picture's file */
        printf("  guide_show: %d then %d, callback \"%s\"\n", g_ret[0], g_ret[1], g_cb);
        CHECK(g_ret[0] == 1, "B (the page's footer handler) ends it through window.nx.endApplet: guide_show returns 1");
        CHECK(g_ret[1] == 2 && !strcmp(g_cb, "http://localhost/changemode?mode=2"), "a callback address ends it with that address: guide_show returns 2");
        CHECK(g_hold == 0 && g_hold_max == 1 && g_closed_calls == 2, "the game is held while it is open and let go after each closing");
        int w = 0, h = 0;
        char shot2[700];
        snprintf(shot2, sizeof shot2, "%s/shot_2.png", g_dir);
        CHECK(all_corners(g_shot, 0xc0, 0x30, 0x20, &w, &h), "the first picture: the page's 1280 x 720 box fills the view (its picture from another folder loaded: red)");
        printf("  pictures: %d x %d\n", w, h);
        CHECK(w >= 1280 && h >= 720 && abs(w * 9 - h * 16) <= 16, "the picture is 16:9 at the window's size or its backing scale (%d x %d)", w, h);
        CHECK(all_corners(shot2, 0x30, 0xc0, 0x40, &w, &h), "after R: the page's footer handler turned it green");
        SDL_DestroyWindow(g_win);
        SDL_Quit();
        printf("guide test: %d of %d checks passed\n", g_pass, g_pass + g_fail);
        return g_fail != 0;
    }
}
