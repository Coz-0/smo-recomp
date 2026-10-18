/* Host display: a real window, 60 Hz pacing and the guest's vsync event.
 * The window's contents are drawn by the graphics layer's render thread (nvn_gl.c's front end; Vulkan by default). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "runtime.h"
#include "hostrt.h"
#include "gfx_options.h"

#define A0 (c->x[0])
#define A1 (c->x[1])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define WIDTH 1280
#define HEIGHT 720
#define FRAME_NS 16666667LL

void os_init_event(uint64_t key, int autoclear);      /* os_threads.c */
void os_signal_event(uint64_t key);

static HWND g_hwnd;
static volatile LONG g_ready;
static volatile LONG64 g_frames;
void nvn_gl_start(void *hwnd);                        /* nvn_gl.c */
void nvn_gl_report(FILE *out);
void nvn_tl(int ev, int arg);                          /* nvn_gl.c: SMO_FRAMETL timeline */
static uint64_t g_vsync_event;
static LARGE_INTEGER g_freq;

/* ONE refresh clock. The game counts the signals of the vsync event on a thread of its own ("VSync Count
 * Thread", 0x7103810a40: window object + 0x138) and, at every present (0x710386fc30), advances its frame counter
 * (0x710593a440) by the number of refreshes counted since the previous present. So the refresh count IS the game's
 * time. Before this, the event came from a Sleep() loop and presentation was held by a second, independent 60 Hz
 * clock that restarted its phase after every late frame: between two presents the game counted 0, 1 or 2 refreshes,
 * and "0" counts as one step while "2" counts as two, so the game's clock gained (10.77 s per 600 frames; measured by
 * ssbu-recomp in Super Smash Bros. Ultimate, whose main the addresses above are in).
 * Now: refresh n happens at t0 + n/60 s; at each one the vsync event is signalled and then the frames waiting to be
 * shown are released. A frame queued between refresh n and n+1 is shown at n+1, and no sooner than `interval`
 * refreshes after the frame before it (nvnWindowSetPresentInterval); the game is held in its wait for the window
 * texture (nvnSyncWait on the sync of nvnWindowAcquireTexture), which is AFTER it has read the refresh count, as on
 * the console. SMO_OLD_PACING=1: the two clocks as before. SMO_CLOCKSTAT=1: one line every 600 presented frames. */
static int g_old_pacing = -1, g_clockstat;
static volatile LONG64 g_vsync_n;            /* refreshes passed (published after the event was signalled) */
static volatile LONG64 g_vsync_signals, g_vsync_late;
static SRWLOCK g_vsync_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE g_vsync_cv = CONDITION_VARIABLE_INIT;
static LONG64 g_latch;                       /* refresh at which the last queued frame is shown */
static volatile LONG g_latch_pending;        /* a frame was queued and nobody has waited for its refresh yet */
static int old_pacing(void) {
    if (g_old_pacing < 0) { const char *e = getenv("SMO_OLD_PACING"); g_old_pacing = e && atoi(e) != 0; g_clockstat = getenv("SMO_CLOCKSTAT") && atoi(getenv("SMO_CLOCKSTAT")) != 0; }
    return g_old_pacing;
}
static void vsync_test_key(void);

/* while the amiibo menu (smo_amiibo.c) holds the game, the game's vsync event is not signalled: its VSync Count
 * Thread would count every refresh of the wait and the game would step that many frames at once when it goes on. The menu
 * paces itself on the same refresh clock (display_menu_wait). */
static volatile LONG g_menu_hold;
void display_menu_hold(int on) { InterlockedExchange(&g_menu_hold, on); }
int display_menu_held(void) { return g_menu_hold != 0; }     /* nvn_vk_watchdog.c: a game held on purpose (menu, guide) is not frozen */
static int vsync_signal_ok(void) { return !g_menu_hold; }

/* TEST ONLY, SMO_SPEED=<n>: the refresh clock (vsync_thread) runs n times faster, 60 x n refreshes a second (any number
 * > 0; 0 = 960 a second, as fast as the PC can run the game). The game waits for a refresh before each frame and steps
 * once per frame, so it runs up to n times faster where the PC keeps up; input sequences count frames, so they play the
 * same. Not set or 1 = nothing changes. (A lockstep clock, a refresh only once the game has queued a frame, never starts
 * this game: it waits for a refresh before its first frame.) */
static double g_rate = 60.0;                          /* refreshes a second */
static void speed_init(void) {
    const char *e = getenv("SMO_SPEED");
    if (!e || !e[0]) return;
    double speed = atof(e);
    g_rate = speed > 0 ? 60.0 * speed : 960.0;
    if (g_rate != 60.0) printf("display: SMO_SPEED=%s (TEST ONLY): the refresh clock runs at %.0f Hz\n", e, g_rate);
}

int oe_exit_request(void);                            /* nnmisc.c */
static volatile LONG g_closing;
int display_closing(void) { return g_closing != 0; }  /* smo_amiibo.c: the menu gives way to the game when the window is being closed */

/* The window's mode (config\graphics.ini, gfx_options.c): windowed, borderless fullscreen, fullscreen (topmost), or the
 * default, a borderless window of the configured size (dragged anywhere to move it). F11, or a double-click in a borderless
 * window, switches between covering the monitor and the window; a window left by a mode that started covering the monitor
 * is the borderless window of the configured size, centred. Called on the window thread. */
static int g_win_full;                                /* the window covers its monitor now */
static int g_win_drag;                                /* the window is a borderless window now: its whole area moves it */
static int g_win_topmost;                             /* graphics.ini fullscreen: covering the monitor means on top */
static RECT g_win_rect;                               /* the window's rectangle (and style) before it covered the monitor */
static DWORD g_win_style;
static void window_fullscreen(HWND h, int on) {
    MONITORINFO mi;
    memset(&mi, 0, sizeof mi); mi.cbSize = sizeof mi;
    if (on == g_win_full || !GetMonitorInfoA(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi)) return;
    if (on) {
        GetWindowRect(h, &g_win_rect);
        g_win_style = (DWORD)GetWindowLongA(h, GWL_STYLE);
        SetWindowLongA(h, GWL_STYLE, (LONG)((g_win_style & ~(DWORD)WS_OVERLAPPEDWINDOW) | WS_POPUP | WS_VISIBLE));
        SetWindowPos(h, g_win_topmost ? HWND_TOPMOST : HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
        g_win_drag = 0;
    } else {
        if (IsRectEmpty(&g_win_rect)) {
            const GfxOptions *go = gfx_options();
            int ww = mi.rcWork.right - mi.rcWork.left, wh = mi.rcWork.bottom - mi.rcWork.top;
            int cw = go->width < ww ? go->width : ww, ch = go->height < wh ? go->height : wh;
            SetRect(&g_win_rect, mi.rcWork.left + (ww - cw) / 2, mi.rcWork.top + (wh - ch) / 2, mi.rcWork.left + (ww - cw) / 2 + cw, mi.rcWork.top + (wh - ch) / 2 + ch);
            g_win_style = WS_POPUP | WS_CLIPCHILDREN;
        }
        SetWindowLongA(h, GWL_STYLE, (LONG)(g_win_style | WS_VISIBLE));
        SetWindowPos(h, HWND_NOTOPMOST, g_win_rect.left, g_win_rect.top, g_win_rect.right - g_win_rect.left, g_win_rect.bottom - g_win_rect.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
        g_win_drag = !(g_win_style & WS_CAPTION);
    }
    g_win_full = on;
    printf("display: the window %s (F11)\n", on ? "covers its monitor" : g_win_drag ? "is a borderless window" : "has its frame again");
    fflush(stdout);
}

void *display_hwnd(void) { return g_hwnd; }
void display_toggle_fullscreen(void) { if (g_hwnd) window_fullscreen(g_hwnd, !g_win_full); }    /* on the window thread (smo_guide.c) */
#define WM_SMO_GUIDE (WM_APP + 0x47)                /* smo_guide.c: the Help page in the game window */
void guide_on_message(HWND hwnd, WPARAM what, LPARAM arg);
void guide_on_size(void);

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_SMO_GUIDE) { guide_on_message(h, w, l); return 0; }
    if (m == WM_SIZE) guide_on_size();
    if (m == WM_NCHITTEST && g_win_drag) {           /* a borderless window: dragging anywhere moves it */
        LRESULT r = DefWindowProcA(h, m, w, l);
        return r == HTCLIENT ? HTCAPTION : r;
    }
    if ((m == WM_NCLBUTTONDBLCLK && w == HTCAPTION && g_win_drag) || (m == WM_LBUTTONDBLCLK && g_win_full)) { window_fullscreen(h, !g_win_full); return 0; }
    if (m == WM_KEYDOWN && w == VK_F11 && !(l & (1 << 30))) { window_fullscreen(h, !g_win_full); return 0; }
    if (m == WM_CLOSE) {                             /* the exit request: nn::oe decides (nnmisc.c oe_exit_request) whether the game ends now or after its exit-request handling section */
        InterlockedExchange(&g_closing, 1);
        printf("\nwindow closed by the user after %lld frames\n", (long long)g_frames);
        if (!oe_exit_request()) report_and_exit(0);
        return 0;
    }
    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_KEYDOWN && w == VK_F12 && !(l & (1 << 30))) {      /* F12 = the console's Capture button (once per press) */
        void smo_capture_request(void);
        smo_capture_request();
        return 0;
    }
    if ((m == WM_KEYDOWN || m == WM_SYSKEYDOWN) && !(l & (1 << 30))) {   /* the amiibo menu's key (controllers.ini [hotkeys], F1 by default) */
        int nnpad_hotkey_vk(int which); void amiibo_menu_request(void);
        int vk = nnpad_hotkey_vk(0);
        if (vk && (int)w == vk) { amiibo_menu_request(); return 0; }
    }
    return DefWindowProcA(h, m, w, l);
}

static char g_title[96];

static DWORD WINAPI window_thread(LPVOID arg) {
    (void)arg;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);   /* smo_guide.c: the WebView2 objects live on this thread */
    WNDCLASSA wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "smo_native_window";
    wc.style = CS_OWNDC | CS_DBLCLKS;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);
    /* window mode and size from config\graphics.ini (gfx_options.c). borderless window (the default): frameless,
     * the configured size (at most the primary monitor's work area), centred on it. borderless fullscreen / fullscreen: a
     * frameless window over the primary monitor (fullscreen: also topmost). F11 switches (window_fullscreen). */
    const GfxOptions *go = gfx_options();
    DWORD style = (go->window_mode ? WS_POPUP : WS_OVERLAPPEDWINDOW) | WS_CLIPCHILDREN;      /* children: the Help page (smo_guide.c) */
    int cw = go->width, ch = go->height, px = CW_USEDEFAULT, py = CW_USEDEFAULT;
    if (go->window_mode) {
        MONITORINFO mi;
        POINT origin = {0, 0};
        memset(&mi, 0, sizeof mi); mi.cbSize = sizeof mi;
        if (GetMonitorInfoA(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            if (go->window_mode == 3) {
                int ww = mi.rcWork.right - mi.rcWork.left, wh = mi.rcWork.bottom - mi.rcWork.top;
                if (cw > ww) cw = ww;
                if (ch > wh) ch = wh;
                px = mi.rcWork.left + (ww - cw) / 2; py = mi.rcWork.top + (wh - ch) / 2;
            } else { cw = mi.rcMonitor.right - mi.rcMonitor.left; ch = mi.rcMonitor.bottom - mi.rcMonitor.top; px = mi.rcMonitor.left; py = mi.rcMonitor.top; }
        }
    }
    g_win_full = go->window_mode == 1 || go->window_mode == 2;
    g_win_drag = go->window_mode == 3;
    g_win_topmost = go->window_mode == 2;
    RECT r = {0, 0, cw, ch};
    AdjustWindowRect(&r, style, FALSE);
    printf("display: window %s, client %dx%d (graphics.ini; F11 switches between covering the monitor and the window)\n",
           go->window_mode == 2 ? "fullscreen (borderless, topmost)" : go->window_mode == 1 ? "borderless fullscreen" : go->window_mode == 3 ? "borderless window" : "windowed", cw, ch);
    /* Only the copies a person plays (smo_native.exe, smo_user.exe, or SMO_SHOW=1) get a window on screen;
     * test copies started under any other name keep theirs off-screen and out of the taskbar, so automated
     * runs do not cover the desktop. SMO_TITLE names the window. */
    char exe[MAX_PATH] = "";
    GetModuleFileNameA(NULL, exe, sizeof exe);
    const char *base = strrchr(exe, '\\') ? strrchr(exe, '\\') + 1 : exe, *show = getenv("SMO_SHOW");
    int visible = show ? atoi(show) != 0 : (!_stricmp(base, "smo_native.exe") || !_stricmp(base, "smo_user.exe"));
    snprintf(g_title, sizeof g_title, "%s", getenv("SMO_TITLE") ? getenv("SMO_TITLE") : "Super Mario Odyssey (smo-recomp)");
    if (visible)
        g_hwnd = CreateWindowExA(go->window_mode == 2 ? WS_EX_TOPMOST : 0, wc.lpszClassName, g_title, style | WS_VISIBLE, px, py, r.right - r.left,
                                 r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    else {
        g_hwnd = CreateWindowExA(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, g_title, style, -32000, -32000,
                                 r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
        ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    }
    InterlockedExchange(&g_ready, 1);
    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    return 0;
}

/* Signals the guest's vsync event 60 times per second. */
static DWORD WINAPI vsync_thread(LPVOID arg) {
    (void)arg;
    LARGE_INTEGER next;
    QueryPerformanceCounter(&next);
    if (!old_pacing()) {
        LARGE_INTEGER t0 = next, now;
        LONG64 n = 0;
        HANDLE timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        for (;;) {
            LONGLONG target = t0.QuadPart + (LONGLONG)((double)(n + 1) * (double)g_freq.QuadPart / g_rate);      /* no accumulated rounding */
            QueryPerformanceCounter(&now);
            LONGLONG left = target - now.QuadPart;
            if (left > 0) {
                LARGE_INTEGER due;
                due.QuadPart = -((left - g_freq.QuadPart / 2000) * 10000000 / g_freq.QuadPart);
                if (due.QuadPart < 0) { if (timer && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) WaitForSingleObject(timer, 100); else Sleep((DWORD)(left * 1000 / g_freq.QuadPart)); }
                for (QueryPerformanceCounter(&now); now.QuadPart < target; QueryPerformanceCounter(&now)) YieldProcessor();
            }
            LONG64 passed = (LONG64)((double)(now.QuadPart - t0.QuadPart) * g_rate / (double)g_freq.QuadPart);   /* >= n + 1; more when this thread was kept off the CPU */
            if (passed < n + 1) passed = n + 1;
            if (passed > n + 1) InterlockedExchangeAdd64(&g_vsync_late, passed - (n + 1));
            n = passed;
            if (g_vsync_event && vsync_signal_ok()) { os_signal_event(g_vsync_event); InterlockedIncrement64(&g_vsync_signals); }
            AcquireSRWLockExclusive(&g_vsync_lock);
            InterlockedExchange64(&g_vsync_n, n);
            ReleaseSRWLockExclusive(&g_vsync_lock);
            WakeAllConditionVariable(&g_vsync_cv);
            nvn_tl(8 /* TL_VSYNC */, (int)(n & 0xffff));
            vsync_test_key();
        }
    }
    for (;;) {
        next.QuadPart += g_freq.QuadPart / 60;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        LONGLONG ms = (next.QuadPart - now.QuadPart) * 1000 / g_freq.QuadPart;
        if (ms > 0) Sleep((DWORD)ms); else next = now;
        if (g_vsync_event && vsync_signal_ok()) { os_signal_event(g_vsync_event); InterlockedIncrement64(&g_vsync_signals); }
        vsync_test_key();
    }
}
static void vsync_test_key(void) {
    {
        /* Test aid: SMO_AUTOKEY=<letter>,<first second>,<every n seconds> presses a key now and then, so a run without a person can get
         * past "press any button". Only while this window is the foreground window, so the key never reaches another program. */
        static int key = -1, first, every, tick, down;
        if (key == -1) {
            const char *e = getenv("SMO_AUTOKEY");
            key = 0;
            if (e && e[0]) { key = e[0]; first = 30; every = 5; sscanf(e + 1, ",%d,%d", &first, &every); if (every < 1) every = 1; }
        }
        tick++;
        if (key && down && tick - down >= 8) { keybd_event((BYTE)key, 0, KEYEVENTF_KEYUP, 0); down = 0; }
        else if (key && !down && tick >= first * 60 && (tick - first * 60) % (every * 60) == 0 && ((tick % 600 == 0 ? (void)printf("display: test key wanted, window foreground: %d%c", GetForegroundWindow() == g_hwnd, 10) : (void)0), 1) && (GetForegroundWindow() == g_hwnd || (SetForegroundWindow(g_hwnd), GetForegroundWindow() == g_hwnd))) { keybd_event((BYTE)key, 0, 0, 0); down = tick; printf("display: test key %c pressed at frame tick %d%c", key, tick, 10); }
    }
}

void hitch_init(void); void hitch_pacing(int on);      /* hitch.c: SMO_HITCH */
void display_init(void) {
    QueryPerformanceFrequency(&g_freq);
    /* the console's waits with a time-out (nn::os::TimedWaitEvent, TimedReceiveMessageQueue, timed condition
     * variables: os_threads.c, SleepConditionVariableCS with whole milliseconds) end at the time asked for. At the
     * default Windows timer period they ended at the next 15.6 ms tick, so a thread that polls with a 1 ms time-out ran
     * 64 times per second: the movie player's two feed threads delivered one sample per tick and its pre-roll loop
     * [game 0x71036f27b4, 125 x 10 ms, holding the player's mutex] ran out every time (1.3 s, main thread blocked).
     * SMO_TIMER_PERIOD=<ms> (default 1; 0 = leave the system default). */
    { const char *e = getenv("SMO_TIMER_PERIOD"); int ms = e ? atoi(e) : 1; if (ms > 0) { MMRESULT r = timeBeginPeriod((UINT)ms); printf("display: system timer period %d ms requested (%s)%c", ms, r == TIMERR_NOERROR ? "ok" : "refused", 10); } }
    hitch_init();
    CreateThread(NULL, 0, window_thread, NULL, 0, NULL);
    while (!g_ready) Sleep(1);
    nvn_gl_start(g_hwnd);
    speed_init();
    CreateThread(NULL, 0, vsync_thread, NULL, 0, NULL);
    { void nvn_vk_selftest_hook(void); nvn_vk_selftest_hook(); }      /* nvn_vk.c: SMO_RENDERER=vk SMO_VK_SELFTEST=<frames>, then exit */
}

/* Called after every presented frame (nvn_gl.c has already put it in the window): holds the caller to 60 Hz. */
static volatile LONG g_interval = 1;         /* nvnWindowSetPresentInterval: display refreshes per presented frame (0 = do not wait) */
void display_set_interval(int n) { InterlockedExchange(&g_interval, n < 0 ? 0 : n > 5 ? 5 : n); }
void nvn_pf_pace(LONG64 ticks);            /* nvn_gl.c: timing scopes */
/* SMO_CLOCKSTAT=1: per 600 presented frames, the real time, the vsync signals sent, the refreshes the game counted
 * (its frame counter at 0x710593a440: 600 = one step per presented frame) and the refreshes this thread slept through. */
static LONG64 g_cs_steps, g_cs_hist[4];      /* game steps (1 + refreshes skipped) and the histogram of "refreshes counted since the last present" 0, 1, 2, 3+ */
/* nvn_gl.c, at nvnWindowAcquireTexture: the game's window object is the index pointer - 0x30; + 0x138 = refreshes its
 * VSync Count Thread has counted, + 0x140 = the count at its previous present. Right after this call the game computes
 * steps = max(count - last, 1) (0x710386fc30) and then stores last = count. */
void display_clock_sample(uint64_t index_ptr) {
    if (!g_clockstat || !index_ptr) return;
    uint64_t count = rd64(index_ptr - 0x30 + 0x138), last = rd64(index_ptr - 0x30 + 0x140);
    LONG64 d = (LONG64)(count - last);
    g_cs_steps += d > 1 ? d : 1;
    g_cs_hist[d <= 0 ? 0 : d >= 3 ? 3 : d]++;
}
static void clock_stat(LONG64 n, LARGE_INTEGER now) {
    static LARGE_INTEGER t_last; static LONG64 sig_last, late_last, steps_last, h_last[4];
    if (!g_clockstat || n % 600) return;
    LONG64 sig = g_vsync_signals, late = g_vsync_late;
    if (t_last.QuadPart) printf("clock: frames %lld-%lld real %.3f s, vsync signals %lld (%.2f Hz), game steps %lld = %.2f s of game time, refreshes counted between presents 0:%lld 1:%lld 2:%lld 3+:%lld, slept through by the vsync thread %lld%c", (long long)(n - 600), (long long)n,
        (double)(now.QuadPart - t_last.QuadPart) / (double)g_freq.QuadPart, (long long)(sig - sig_last), (double)(sig - sig_last) * (double)g_freq.QuadPart / (double)(now.QuadPart - t_last.QuadPart), (long long)(g_cs_steps - steps_last), (double)(g_cs_steps - steps_last) / 60.0,
        (long long)(g_cs_hist[0] - h_last[0]), (long long)(g_cs_hist[1] - h_last[1]), (long long)(g_cs_hist[2] - h_last[2]), (long long)(g_cs_hist[3] - h_last[3]), (long long)(late - late_last), 10);
    t_last = now; sig_last = sig; late_last = late; steps_last = g_cs_steps; memcpy(h_last, g_cs_hist, sizeof h_last);
}
/* New pacing, part 1 (nvnQueuePresentTexture, after the frame is in the window): decides the refresh that shows it. */
static void frame_queued(void) {
    static LARGE_INTEGER second_start;
    static LONG64 second_frames;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!second_start.QuadPart) second_start = now;
    LONG64 n = InterlockedIncrement64(&g_frames);
    second_frames++;
    LONG64 at = g_vsync_n + 1, iv = g_interval;
    if (!iv) at = g_vsync_n;                                       /* interval 0: shown at once, nobody waits */
    else if (at < g_latch + iv) at = g_latch + iv;
    g_latch = at;
    InterlockedExchange(&g_latch_pending, iv != 0);
    clock_stat(n, now);
    {   /* TEST ONLY, SMO_FRAMELOG=<k>: a line "frame-mark: <n>" every k presented frames, to place log events in game frames */
        static int every = -1;
        if (every < 0) { const char *e = getenv("SMO_FRAMELOG"); every = e ? atoi(e) : 0; }
        if (every > 0 && n % every == 0) printf("frame-mark: %lld%c", (long long)n, 10);
    }
    if ((now.QuadPart - second_start.QuadPart) >= g_freq.QuadPart) {
        char title[160];
        if (gfx_options()->overlay && second_frames) {       /* frame_time_overlay = on */
            double ms = (double)(now.QuadPart - second_start.QuadPart) * 1000.0 / (double)g_freq.QuadPart / (double)second_frames;
            printf("frame-time: %.2f ms per frame over the last second (%lld frames)\n", ms, (long long)second_frames);
            snprintf(title, sizeof title, "%s - frame %lld - %lld fps - %.2f ms per frame", g_title, (long long)n, (long long)second_frames, ms);
        } else
        snprintf(title, sizeof title, "%s - frame %lld - %lld fps", g_title, (long long)n, (long long)second_frames);
        SetWindowTextA(g_hwnd, title);
        second_frames = 0;
        second_start = now;
    }
}
/* New pacing, part 2: holds the caller until the refresh that shows the last queued frame. Returns 1 if it waited. */
int display_wait_refresh(void) {
    if (old_pacing() || !InterlockedExchange(&g_latch_pending, 0)) return 0;
    if (g_vsync_n >= g_latch) return 0;
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    hitch_pacing(1);
    nvn_tl(6 /* TL_PACE_BEGIN */, (int)(g_latch & 0xffff));
    AcquireSRWLockExclusive(&g_vsync_lock);
    while (g_vsync_n < g_latch) SleepConditionVariableSRW(&g_vsync_cv, &g_vsync_lock, 100, 0);
    ReleaseSRWLockExclusive(&g_vsync_lock);
    nvn_tl(7 /* TL_PACE_END */, (int)(g_vsync_n & 0xffff));
    hitch_pacing(0);
    QueryPerformanceCounter(&b);
    nvn_pf_pace(b.QuadPart - a.QuadPart);
    return 1;
}
/* smo_amiibo.c: the menu's own pace while it holds the game: until the next refresh of the clock (16 ms where there is no
 * refresh clock: SMO_OLD_PACING) */
void display_menu_wait(void) {
    if (old_pacing()) { Sleep(16); return; }
    LONG64 n = g_vsync_n;
    AcquireSRWLockExclusive(&g_vsync_lock);
    while (g_vsync_n == n) if (!SleepConditionVariableSRW(&g_vsync_cv, &g_vsync_lock, 100, 0)) break;
    ReleaseSRWLockExclusive(&g_vsync_lock);
}
/* Before a present: a frame queued earlier that nobody waited for (a caller that does not wait on the window sync). */
void display_before_present(void) {
    static int said;
    if (old_pacing() || !g_latch_pending) return;
    if (!said) { said = 1; printf("display: a present without a wait on the window texture's sync: held at the next present instead%c", 10); }
    display_wait_refresh();
}
void display_pace(void) {
    static LARGE_INTEGER next, second_start;
    static LONG64 second_frames;
    LARGE_INTEGER now;
    if (!old_pacing()) { frame_queued(); return; }
    QueryPerformanceCounter(&now);
    if (!next.QuadPart) { next = now; second_start = now; }
    LONG64 n = InterlockedIncrement64(&g_frames);
    second_frames++;
    clock_stat(n, now);
    next.QuadPart += g_freq.QuadPart * g_interval / 60;
    /* wait until `next` exactly. Sleep(ms) cut the wait to whole milliseconds and, at the default 15.6 ms timer period,
     * could overshoot by a frame; a frame that came late was then followed by frames shown early (up to 100 ms of debt).
     * Now: a high-resolution waitable timer up to 0.5 ms before the deadline, then a spin; a late frame may be made up for
     * by at most one refresh interval, as a display that missed a refresh shows the next frame at the next one. */
    LONGLONG left = next.QuadPart - now.QuadPart, frame_ticks = g_freq.QuadPart * (g_interval ? g_interval : 1) / 60;
    if (!g_interval) next = now;
    else if (left > 0) {
        static HANDLE timer;
        LARGE_INTEGER after, due;
        hitch_pacing(1);
        if (!timer) timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
        due.QuadPart = -((left - g_freq.QuadPart / 2000) * 10000000 / g_freq.QuadPart);          /* relative, 100 ns units */
        if (due.QuadPart < 0) { if (timer && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) WaitForSingleObject(timer, 100); else Sleep((DWORD)(left * 1000 / g_freq.QuadPart)); }
        for (QueryPerformanceCounter(&after); after.QuadPart < next.QuadPart; QueryPerformanceCounter(&after)) YieldProcessor();
        hitch_pacing(0);
        nvn_pf_pace(after.QuadPart - now.QuadPart);
    } else if (-left > frame_ticks) next.QuadPart = now.QuadPart - frame_ticks;
    if ((now.QuadPart - second_start.QuadPart) >= g_freq.QuadPart) {
        char title[160];
        if (gfx_options()->overlay && second_frames) printf("frame-time: %.2f ms per frame over the last second (%lld frames)\n", (double)(now.QuadPart - second_start.QuadPart) * 1000.0 / (double)g_freq.QuadPart / (double)second_frames, (long long)second_frames);
        if (gfx_options()->overlay && second_frames) snprintf(title, sizeof title, "%s - frame %lld - %lld fps - %.2f ms per frame", g_title, (long long)n, (long long)second_frames, (double)(now.QuadPart - second_start.QuadPart) * 1000.0 / (double)g_freq.QuadPart / (double)second_frames);
        else
        snprintf(title, sizeof title, "%s - frame %lld - %lld fps", g_title, (long long)n, (long long)second_frames);
        SetWindowTextA(g_hwnd, title);
        second_frames = 0;
        second_start = now;
    }
}

long long display_frames(void) { return g_frames; }  /* smo_nerve.c (SMO_STAGE_LIST): frames presented so far */

void display_report(FILE *out) {
    fprintf(out, "\ndisplay: %lld frames presented\n", (long long)g_frames);
    fprintf(out, "display: %s; %lld vsync signals, %lld refreshes slept through by the vsync thread\n", old_pacing() ? "old pacing (SMO_OLD_PACING=1: two clocks)" : "one refresh clock", (long long)g_vsync_signals, (long long)g_vsync_late);
    nvn_gl_report(out);
}

/* ---- nn::vi: one display, one layer; the native window is an opaque token for NVN */
static uint8_t g_display_token[16], g_layer_token[16], g_native_window_token[16];
static void vi_OpenDefaultDisplay(Ctx *c) { wr64(A0, (uint64_t)(uintptr_t)g_display_token); RET(0); }
static void vi_CreateLayer(Ctx *c) { wr64(A0, (uint64_t)(uintptr_t)g_layer_token); RET(0); }
static void vi_GetNativeWindow(Ctx *c) { wr64(A0, (uint64_t)(uintptr_t)g_native_window_token); RET(0); }
static void vi_GetDisplayVsyncEvent(Ctx *c) {         /* (SystemEventType *out, Display *) */
    os_init_event(A0, 1);
    g_vsync_event = A0;
    RET(0);
}
static void nothing(Ctx *c) { (void)c; }

static const ImportDef vi_imports[] = {
    {"_ZN2nn2vi10InitializeEv", nothing},
    {"_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", vi_OpenDefaultDisplay},
    {"_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", vi_CreateLayer},
    {"_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", vi_GetNativeWindow},
    {"_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE", vi_GetDisplayVsyncEvent},
};

void register_display_imports(void) {
    register_imports(vi_imports, (int)(sizeof vi_imports / sizeof vi_imports[0]));
}
