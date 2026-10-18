/* Small SDK services: nn::oe notifications, focus and language, nn::prepo, nn::err, nn::crypto hashes, the C-linkage
 * nnos* wrappers, and "offline console" answers for the network-facing services (friends, account, socket byte order).
 *
 * Where a behaviour or layout is stated here it was read from one of:
 *   [sdk]   the real SDK module (Super Smash Bros. Ultimate 13.0.4's, read by ssbu-recomp), disassembled by symbol name;
 *   [game]  the game's own code at the quoted address;
 *   [doc]   public documentation (switchbrew / open-ead nnheaders), not checked against code.
 * Everything else only returns "success", "nothing" or "not available" and touches no
 * guest structure.
 *
 * Registration never replaces a handler another file already installed, except for the names of replace_imports
 * (below). */
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <windows.h>
#include <wincrypt.h>
#include "runtime.h"
#include "hostrt.h"

#pragma comment(lib, "advapi32.lib")

#define A0 (c->x[0])
#define A1 (c->x[1])
#define A2 (c->x[2])
#define A3 (c->x[3])
#define A4 (c->x[4])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define STR(n) ((const char *)(uintptr_t)c->x[n])

/* Some non-zero nn::Result. The SDK's real "not available" codes differ per service and are
 * not established; the game only tests these for failure. Same value as nnsdk.c. */
#define RESULT_UNAVAILABLE 0x3e87aull

void os_init_event(uint64_t key, int autoclear);          /* os_threads.c */
void os_signal_event(uint64_t key);

static void keep(Ctx *c) { (void)c; }                     /* void function, or returns `this` */
static void ret0(Ctx *c) { RET(0); }                      /* Result success / false / 0 / null */
static void ret1(Ctx *c) { RET(1); }
static void unavailable(Ctx *c) { RET(RESULT_UNAVAILABLE); }
/* nn::account network-service-account functions on a machine without a linked account: see nnfs.c ([sdk] 0x4b3a8,
 * account results 124-0200..0269 mean "no usable network service account"). */
static void acc_nsa_unavailable(Ctx *c) { RET(0x1907cull); }

static void fatal(Ctx *c, const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    printf("\n%s (guest caller 0x%llx)\n", msg, (unsigned long long)c->x[30] - 4);
    trace_printf("%s (guest caller 0x%llx)\n", msg, (unsigned long long)c->x[30] - 4);
    report_and_exit(3);
}
static const char *safe_str(uint64_t p) { return p > 0x10000 ? (const char *)(uintptr_t)p : "(null)"; }

/* ---------------------------------------------------------------- C-linkage nn::os wrappers
 * [sdk] every nnosXxx is a tail jump to the C++ function with the same arguments, so each one
 * forwards to whatever handler is registered for the C++ name (looked up on first use; also when the game
 * does not import the C++ name itself). */
static ImportFn forward_target(Ctx *c, const char *name) {
    ImportFn f = import_lookup(name);
    if (!f) f = handler_by_name(name);
    if (!f) fatal(c, "nnmisc: no handler for %s, needed by a C wrapper", name);
    return f;
}
#define FWD(fn, target) static void fn(Ctx *c) { static ImportFn f; if (!f) f = forward_target(c, target); f(c); }
#define OS(n) "_ZN2nn2os" n
FWD(nnos_GetCurrentThread, OS("16GetCurrentThreadEv"))
FWD(nnos_CreateThread, OS("12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mi"))
FWD(nnos_StartThread, OS("11StartThreadEPNS0_10ThreadTypeE"))
FWD(nnos_WaitThread, OS("10WaitThreadEPNS0_10ThreadTypeE"))
FWD(nnos_DestroyThread, OS("13DestroyThreadEPNS0_10ThreadTypeE"))
FWD(nnos_GetThreadPriority, OS("17GetThreadPriorityEPKNS0_10ThreadTypeE"))
FWD(nnos_GetThreadCoreMask, OS("17GetThreadCoreMaskEPiPmPKNS0_10ThreadTypeE"))
FWD(nnos_SetThreadCoreMask, OS("17SetThreadCoreMaskEPNS0_10ThreadTypeEim"))
FWD(nnos_SetThreadName, OS("13SetThreadNameEPNS0_10ThreadTypeEPKc"))      /* the host keeps a copy either way */
FWD(nnos_SleepThread, OS("11SleepThreadENS_8TimeSpanE"))
FWD(nnos_InitializeMutex, OS("15InitializeMutexEPNS0_9MutexTypeEbi"))
FWD(nnos_FinalizeMutex, OS("13FinalizeMutexEPNS0_9MutexTypeE"))
FWD(nnos_LockMutex, OS("9LockMutexEPNS0_9MutexTypeE"))
FWD(nnos_UnlockMutex, OS("11UnlockMutexEPNS0_9MutexTypeE"))
FWD(nnos_InitializeEvent, OS("15InitializeEventEPNS0_9EventTypeEbNS0_14EventClearModeE"))
FWD(nnos_FinalizeEvent, OS("13FinalizeEventEPNS0_9EventTypeE"))
FWD(nnos_SignalEvent, OS("11SignalEventEPNS0_9EventTypeE"))
FWD(nnos_WaitEvent, OS("9WaitEventEPNS0_9EventTypeE"))
FWD(nnos_GetSystemTick, OS("13GetSystemTickEv"))
FWD(nnos_GetSystemTickFrequency, OS("22GetSystemTickFrequencyEv"))
FWD(nnos_ConvertTickToNanoSeconds, OS("17ConvertToTimeSpanENS0_4TickE"))

/* ---------------------------------------------------------------- abort helpers (never return on the console) */
static void diag_AbortImpl(Ctx *c) {                      /* (condition, function, file, line) */
    printf("\nnn::diag abort: %s  in %s  (%s:%d)\n", safe_str(A0), safe_str(A1), safe_str(A2), (int)A3);
    trace_printf("nn::diag abort: %s  in %s  (%s:%d)\n", safe_str(A0), safe_str(A1), safe_str(A2), (int)A3);
    guest_trap("guest called nn::diag::detail::AbortImpl", c->x[30] - 4);
}
static void diag_AbortFormatImpl(Ctx *c) {                /* (condition, function, file, line, format, ...) */
    printf("\nnn::diag abort: %s  in %s  (%s:%d)  format \"%s\"\n", safe_str(A0), safe_str(A1), safe_str(A2), (int)A3, safe_str(A4));
    trace_printf("nn::diag abort: %s  in %s  (%s:%d)  format \"%s\"\n", safe_str(A0), safe_str(A1), safe_str(A2), (int)A3, safe_str(A4));
    guest_trap("guest called nndetailDiagAbortFormatImpl", c->x[30] - 4);
}
static void diag_UnexpectedDefault(Ctx *c) {              /* (function, file, line) */
    printf("\nnn unexpected default: %s  (%s:%d)\n", safe_str(A0), safe_str(A1), (int)A2);
    trace_printf("nn unexpected default: %s  (%s:%d)\n", safe_str(A0), safe_str(A1), (int)A2);
    guest_trap("guest called nn::detail::UnexpectedDefaultImpl", c->x[30] - 4);
}

/* ---------------------------------------------------------------- nn::oe */
static LARGE_INTEGER g_start;
/* Notification messages. On the console the system posts a message when the application's focus state changes
 * (HOME menu, an overlay), when the console is docked or undocked, and when it asks the application to exit; the
 * application reads them with TryPopNotificationMessage and may wait on the event GetNotificationMessageEvent
 * returns, which is signalled while messages are pending.
 * Here: the focus state follows the game window. When the window stops being the foreground window the state
 * becomes OutOfFocus (2) and MessageFocusStateChanged (15) is posted; back in the foreground: InFocus (1) and the
 * message again. [sdk] 0x32c3d0: SetFocusHandlingMode passes "notify focus changes" = true for the modes 1
 * (NoSuspend) and 2 (SuspendHomeSleepNotify) only, so the message is posted only in those modes.
 * The operation mode never changes on a PC (always the TV mode), so MessageOperationModeChanged (30) and
 * MessagePerformanceModeChanged (31) never occur; the enable flags are kept as the game sets them.
 * (the state now follows the applet manager's table, see oe_track_focus: not in focus = 2 only while the window is
 * visible; minimised = 3 for a NoSuspend application and no change at all in the suspending modes.)
 * Differences from the console, stated: (1) in the suspending modes (0, 2, 3) the console also FREEZES the application
 * while it is in the background (mode 3: also while obscured); the runtime does not freeze the game's threads: a minimised
 * game keeps running. (2) A window
 * that has never been in the foreground (the automated tests keep theirs off screen) is treated as in focus.
 * (3) Closing the window is the exit request: see oe_exit_request below.
 * SMO_FOCUS_NOTIFY=0 turns the focus tracking off. */
#define OE_MSG_FOCUS_STATE_CHANGED 15u
static uint8_t g_oe_event[0x40];
static uint32_t g_oe_queue[16];
static int g_oe_count, g_oe_event_ready, g_oe_focus_state = 1, g_oe_focus_mode = 0, g_oe_was_foreground;
static int g_oe_opmode_notify, g_oe_resume_notify, g_oe_focus_pending, g_oe_focus_recheck;
static HWND g_oe_window;
static CRITICAL_SECTION g_oe_lock;
static void oe_event_setup(void) {
    if (!g_oe_event_ready) { os_init_event((uint64_t)(uintptr_t)g_oe_event, 0); g_oe_event_ready = 1; }
}
static void oe_push(uint32_t message) {
    EnterCriticalSection(&g_oe_lock);
    oe_event_setup();
    if (g_oe_count < 16) g_oe_queue[g_oe_count++] = message;
    os_signal_event((uint64_t)(uintptr_t)g_oe_event);
    LeaveCriticalSection(&g_oe_lock);
}
/* Suspension. The applet manager suspends the application's process while it is not runnable (suyu am/applet.cpp
 * UpdateSuspensionStateLocked, am/lifecycle_manager.cpp IsRunnable): in the background unless the handling mode is NoSuspend,
 * obscured only in AlwaysSuspend; never while an exit request is pending. A message "Resume" (16) is queued at that moment when
 * the application enabled it (RequestResumeNotification). Here a watcher thread applies the rule to the window every 20 ms and
 * sets hostrt.c's g_guest_frozen: every guest thread then stops at its next SDK call or return (hostrt.c host_import).
 * SMO_FREEZE_TEST=<start s>,<seconds> minimises and restores the window by itself (test; the log line counts the polls of the
 * game's main loop during the freeze). */
extern volatile LONG g_guest_frozen;
static volatile LONG g_oe_poll_count;
static int g_oe_exit_requested;                           /* defined with the exit request code below */
#define OE_MSG_RESUME 16u
static BOOL CALLBACK oe_find_window(HWND w, LPARAM out) {
    DWORD pid = 0;
    if (GetWindowThreadProcessId(w, &pid) && pid == GetCurrentProcessId() && IsWindowVisible(w) && !GetWindow(w, GW_OWNER)) { *(HWND *)out = w; return FALSE; }
    return TRUE;
}
static DWORD WINAPI oe_watch(LPVOID unused) {
    const char *test = getenv("SMO_FREEZE_TEST");
    double t_start = 0, t_len = 0;
    int test_phase = 0;
    ULONGLONG t0 = GetTickCount64(), frozen_at = 0;
    LONG polls_at = 0;
    if (test) sscanf(test, "%lf,%lf", &t_start, &t_len);
    for (;;) {
        Sleep(20);
        double t = (double)(GetTickCount64() - t0) / 1000.0;
        if (test && test_phase == 0 && t >= t_start) {
            HWND w = NULL;
            EnumWindows(oe_find_window, (LPARAM)&w);
            if (w) { g_oe_window = w; g_oe_was_foreground = 1; ShowWindow(w, SW_SHOWMINNOACTIVE); test_phase = 1; printf("nn::oe: freeze test: window minimised (handling mode %d)\n", g_oe_focus_mode); fflush(stdout); }
        } else if (test_phase == 1 && t >= t_start + t_len) {
            ShowWindow(g_oe_window, SW_SHOWNOACTIVATE);
            test_phase = 2;
        }
        DWORD pid = 0;
        HWND fg = GetForegroundWindow();
        int ours = fg && GetWindowThreadProcessId(fg, &pid) && pid == GetCurrentProcessId();
        if (ours) { g_oe_was_foreground = 1; g_oe_window = fg; }
        if (!g_oe_was_foreground) continue;
        int minimised = g_oe_window && IsIconic(g_oe_window);
        int runnable = ours || g_oe_exit_requested || (minimised ? g_oe_focus_mode == 1 : g_oe_focus_mode != 3);
        if (!runnable && !g_guest_frozen) {
            if (g_oe_resume_notify) oe_push(OE_MSG_RESUME);
            frozen_at = GetTickCount64(); polls_at = g_oe_poll_count;
            InterlockedExchange(&g_guest_frozen, 1);
            trace_printf("     nn::oe: application suspended (%s, handling mode %d)\n", minimised ? "background" : "obscured", g_oe_focus_mode);
        } else if (runnable && g_guest_frozen) {
            LONG polls = g_oe_poll_count - polls_at;
            InterlockedExchange(&g_guest_frozen, 0);
            printf("nn::oe: application resumed after %llu ms of suspension; main-loop polls during it: %ld\n", (unsigned long long)(GetTickCount64() - frozen_at), (long)polls);
            fflush(stdout);
            trace_printf("     nn::oe: application resumed after %llu ms\n", (unsigned long long)(GetTickCount64() - frozen_at));
        }
        if (test_phase == 2 && !g_guest_frozen) { g_oe_was_foreground = 0; test_phase = 3; }      /* the test window was never the foreground window: back to "in focus" */
    }
    return 0;
}
static void oe_track_focus(void) {
    static int enabled = -1;
    static ULONGLONG last;
    static volatile LONG watcher;
    ULONGLONG now = GetTickCount64();
    if (enabled < 0) { const char *e = getenv("SMO_FOCUS_NOTIFY"); enabled = !(e && e[0] == '0'); }
    if (enabled && InterlockedCompareExchange(&watcher, 1, 0) == 0) CloseHandle(CreateThread(NULL, 0, oe_watch, NULL, 0, NULL));
    if (!enabled || (now - last < 30 && !g_oe_focus_recheck)) return;
    last = now;
    g_oe_focus_recheck = 0;
    DWORD pid = 0;
    HWND fg = GetForegroundWindow();
    int ours = fg && GetWindowThreadProcessId(fg, &pid) && pid == GetCurrentProcessId();
    if (ours) { g_oe_was_foreground = 1; g_oe_window = fg; }
    if (!g_oe_was_foreground) return;
    /* the applet manager's rule (suyu am/lifecycle_manager.cpp UpdateRequestedFocusState, GetFocusStateWhile*), with
     * the window as the activity state: foreground = ForegroundVisible; visible but not the foreground window = ForegroundObscured
     * (something else has the input, as under an overlay or a library applet); minimised = Background (as behind the HOME menu).
     * The handling mode is what [sdk 0x32c3d0, table 0xa7ff70] sends for the oe mode: 0 = (notify 0, suspend 1), 1 = (notify 1,
     * suspend 0) NoSuspend, 2 = (notify 1, suspend 1), 3 = (notify 0, suspend 1) + SetOutOfFocusSuspendingEnabled(1) AlwaysSuspend. */
    int minimised = g_oe_window && IsIconic(g_oe_window);
    int state = ours ? 1                                                    /* InFocus */
              : g_oe_focus_mode == 3 ? 1                                    /* AlwaysSuspend: the application never learns it */
              : !minimised ? 2                                              /* obscured: NotInFocus in the other three modes */
              : g_oe_focus_mode == 1 ? 3 : 1;                               /* background: Background (3) for a NoSuspend application, else unchanged */
    if (state != g_oe_focus_state) {
        g_oe_focus_state = state;
        g_oe_focus_pending = 1;                                             /* m_has_focus_state_changed */
        trace_printf("     nn::oe: focus state -> %d (%s; handling mode %d)\n", state, state == 1 ? "in focus" : state == 2 ? "not in focus" : "background", g_oe_focus_mode);
    }
    if (g_oe_focus_pending && (g_oe_focus_mode == 1 || g_oe_focus_mode == 2)) { g_oe_focus_pending = 0; oe_push(OE_MSG_FOCUS_STATE_CHANGED); }   /* delivered only while notification is enabled */
}
/* Exit request. [sdk 0x32cc20 / 0x32cca0] Enter / LeaveExitRequestHandlingSection keep a nesting count under a mutex and call the
 * applet manager's ISelfController LockExit when the count goes 0 -> 1 and UnlockExit when it goes 1 -> 0. The applet manager is a
 * system module (not in the dump); its rule, from the reference implementation (suyu am/service/self_controller.cpp LockExit /
 * UnlockExit, application_accessor.cpp RequestExit):
 *   exit requested while the exit is locked   -> the application is NOT ended; it gets MessageExitRequest (4) and keeps running
 *   exit requested while it is not locked     -> the process is terminated at once
 *   LockExit after an exit request            -> terminated at once
 *   UnlockExit after an exit request          -> terminated
 * Here the exit request is the user closing the game window (display.c WM_CLOSE calls oe_exit_request). */
#define OE_MSG_EXIT_REQUEST 4u
static int g_oe_exit_locked, g_oe_exit_requested;
int oe_exit_request(void) {                              /* display.c, window thread. Returns 1 when the game keeps running until it leaves its section */
    int locked;
    EnterCriticalSection(&g_oe_lock);
    locked = g_oe_exit_locked;
    if (locked) g_oe_exit_requested = 1;
    LeaveCriticalSection(&g_oe_lock);
    if (!locked) return 0;
    trace_printf("     nn::oe: exit requested inside an exit-request handling section: MessageExitRequest posted\n");
    oe_push(OE_MSG_EXIT_REQUEST);
    return 1;
}
static void oe_TryPopNotificationMessage(Ctx *c) {       /* (uint32_t *out) -> bool */
    uint64_t out = A0;
    int got = 0;
    InterlockedIncrement(&g_oe_poll_count);
    oe_track_focus();
    EnterCriticalSection(&g_oe_lock);
    if (g_oe_count > 0) {
        if (out) wr32(out, g_oe_queue[0]);
        memmove(g_oe_queue, g_oe_queue + 1, (size_t)(--g_oe_count) * sizeof g_oe_queue[0]);
        got = 1;
        if (g_oe_count == 0 && g_oe_event_ready) os_init_event((uint64_t)(uintptr_t)g_oe_event, 0);   /* nothing pending: event cleared */
    }
    LeaveCriticalSection(&g_oe_lock);
    RET(got);
}
static void oe_GetCurrentFocusState(Ctx *c) { oe_track_focus(); RET(g_oe_focus_state); }
static void oe_SetFocusHandlingMode(Ctx *c) { g_oe_focus_mode = (int)(uint32_t)A0; g_oe_focus_recheck = 1; oe_track_focus(); }               /* 0 SuspendHomeSleep, 1 NoSuspend, 2 SuspendHomeSleepNotify, 3 AlwaysSuspend */
static void oe_SetOperationModeChangedNotificationEnabled(Ctx *c) { g_oe_opmode_notify = (int)(A0 & 1); }

/* ---------------------------------------------------------------- nn::settings
 * [doc] nn::settings::Language values and their codes (switchbrew "Settings services"); the
 * order agrees with what nnsdk.c already uses (4 = "it"). LanguageCode is 8 chars, returned in x0. */
static const char *const g_language_codes[] = {"ja", "en-US", "fr", "de", "it", "es", "zh-CN", "ko", "nl", "pt", "ru",
                                               "zh-TW", "en-GB", "fr-CA", "es-419", "zh-Hans", "zh-Hant", "pt-BR"};
static uint64_t language_code(uint32_t lang) {
    uint64_t v = 0;
    if (lang < sizeof g_language_codes / sizeof g_language_codes[0]) memcpy(&v, g_language_codes[lang], strlen(g_language_codes[lang]));
    return v;
}
static void settings_LanguageEqualsCode(Ctx *c) {         /* operator==(const Language&, const LanguageCode&) */
    char a[9] = {0}, b[9] = {0};
    uint64_t v = language_code(rd32(A0));
    memcpy(a, &v, 8);
    memcpy(b, (void *)(uintptr_t)A1, 8);
    RET(v != 0 && strcmp(a, b) == 0);
}
static void settings_CodeEqualsLanguage(Ctx *c) {         /* operator==(const LanguageCode&, const Language&): the same comparison, arguments swapped */
    uint64_t t = c->x[0];
    c->x[0] = c->x[1];
    c->x[1] = t;
    settings_LanguageEqualsCode(c);
}
/* nn::oe::GetDesiredLanguage: the language the application should use = the system language when the application
 * supports it, otherwise the system's choice among the supported ones. The system language here is the Windows
 * display language of the user. Languages this game ships (its message files): ja, en-US, en-GB, fr, fr-CA, de, it,
 * es, es-419, zh-Hans, zh-Hant, ko, nl, ru; anything else gives en-US (NOT VERIFIED: the console decides from the
 * supported-language mask of the application's control data, which was not read here).
 * SMO_LANGUAGE=<code> overrides (for example SMO_LANGUAGE=en-US). */
static uint64_t desired_language(void) {
    static uint64_t cached;
    static const char *const supported[] = {"ja", "en-US", "en-GB", "fr", "fr-CA", "de", "it", "es", "es-419", "zh-Hans", "zh-Hant", "ko", "nl", "ru"};
    if (cached) return cached;
    char name[LOCALE_NAME_MAX_LENGTH * 3] = "", pick[16] = "en-US";
    const char *env = getenv("SMO_LANGUAGE");
    wchar_t wname[LOCALE_NAME_MAX_LENGTH] = L"";
    ULONG count = 0, size = 0;
    if (env && env[0]) snprintf(name, sizeof name, "%s", env);
    else {
        /* the display language of the user interface, not the regional format */
        wchar_t list[512];
        size = 512;
        if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, list, &size) && count > 0) wcsncpy(wname, list, LOCALE_NAME_MAX_LENGTH - 1);
        else GetUserDefaultLocaleName(wname, LOCALE_NAME_MAX_LENGTH);
        WideCharToMultiByte(CP_UTF8, 0, wname, -1, name, sizeof name, NULL, NULL);
    }
    int exact = 0;
    for (size_t i = 0; i < sizeof supported / sizeof supported[0]; i++) if (!_stricmp(name, supported[i])) { strcpy(pick, supported[i]); exact = 1; }
    if (!exact) {
        if (!_strnicmp(name, "zh", 2)) strcpy(pick, strstr(name, "TW") || strstr(name, "HK") || strstr(name, "MO") || strstr(name, "Hant") ? "zh-Hant" : "zh-Hans");
        else if (!_strnicmp(name, "en", 2)) strcpy(pick, !_stricmp(name, "en-US") || !_stricmp(name, "en-CA") ? "en-US" : "en-GB");
        else if (!_strnicmp(name, "fr", 2)) strcpy(pick, !_stricmp(name, "fr-CA") ? "fr-CA" : "fr");
        else if (!_strnicmp(name, "es", 2)) strcpy(pick, !_stricmp(name, "es-ES") || !_stricmp(name, "es") ? "es" : "es-419");
        else {
            char two[3] = {name[0], name[1], 0};
            for (size_t i = 0; i < sizeof supported / sizeof supported[0]; i++) if (!_stricmp(two, supported[i])) strcpy(pick, supported[i]);
        }
    }
    uint64_t v = 0;
    memcpy(&v, pick, strlen(pick));
    printf("nn::oe::GetDesiredLanguage: Windows display language \"%s\" -> \"%s\"\n", name, pick);
    cached = v;
    return v;
}
static void oe_GetDesiredLanguage(Ctx *c) { RET(desired_language()); }   /* LanguageCode (8 chars) by value in x0 */
/* smo_amiibo.c: the same language as text ("en-US", ...), for the game's message files of the host menus */
const char *oe_language(void) { static char code[9]; uint64_t v = desired_language(); memcpy(code, &v, 8); code[8] = 0; return code; }

/* ---------------------------------------------------------------- nn::err
 * The console's error viewer shows "Error Code: 2MMM-DDDD" (2000 + module, description) in a dialog and returns when
 * the user closes it. Here it is a Windows message box on the calling thread, which blocks the same way. Scripted
 * runs (SMO_SEQ / SMO_PLAY / SMO_PRESS / SMO_SCRIPT) and SMO_ERROR_DIALOG=0 only log, so tests never hang. */
static void err_dialog(unsigned category, unsigned number) {
    const char *off = getenv("SMO_ERROR_DIALOG");
    char text[160];
    if ((off && off[0] == '0') || getenv("SMO_SEQ") || getenv("SMO_PLAY") || getenv("SMO_PRESS") || getenv("SMO_SCRIPT")) return;
    snprintf(text, sizeof text, "Error Code: %04u-%04u\n\nAn error has occurred.", category, number);
    MessageBoxA(NULL, text, "Error", MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TASKMODAL);
}
static void err_ShowErrorResult(Ctx *c) {                 /* (Result): the console shows the error applet */
    uint32_t r = (uint32_t)A0;
    err_dialog(2000 + (r & 0x1ff), (r >> 9) & 0x1fff);
    printf("nn::err::ShowError: result 0x%x (module %u, description %u), caller 0x%llx\n", r, r & 0x1ff, (r >> 9) & 0x1fff,
           (unsigned long long)c->x[30] - 4);
    trace_printf("     nn::err::ShowError result 0x%x (module %u, description %u)\n", r, r & 0x1ff, (r >> 9) & 0x1fff);
}

/* ---------------------------------------------------------------- nn::util */
static HCRYPTPROV crypt_provider(void) {
    static HCRYPTPROV prov;
    static volatile LONG lock;
    while (InterlockedExchange(&lock, 1)) SwitchToThread();
    if (!prov && !CryptAcquireContextA(&prov, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) prov = 0;
    InterlockedExchange(&lock, 0);
    return prov;
}
/* [sdk] BinaryFileHeader checks (layout in nnsdk.c): signature = first 8 bytes; alignment shift u8 @0xe;
 * byte-order mark i16 @0xc, reversed when it reads 0xFFFE. */
static void util_BinaryFileHeader_IsSignatureValid(Ctx *c) { RET(rd64(A0) == A1); }
static void util_BinaryFileHeader_IsAlignmentValid(Ctx *c) { RET((A0 & ~(~0ull << (rd8(A0 + 0xe) & 63))) == 0); }
static void util_BinaryFileHeader_IsEndianReverse(Ctx *c) { RET(rd16(A0 + 0xc) == 0xfffe); }

/* ---------------------------------------------------------------- nn::crypto (Windows CryptoAPI) */
static void crypto_Sha1(Ctx *c) {                         /* (void *out, size_t outSize, const void *data, size_t size) */
    HCRYPTHASH h = 0;
    uint8_t digest[20] = {0};
    DWORD len = sizeof digest;
    if (crypt_provider() && CryptCreateHash(crypt_provider(), CALG_SHA1, 0, 0, &h)) {
        CryptHashData(h, (const BYTE *)(uintptr_t)A2, (DWORD)A3, 0);
        CryptGetHashParam(h, HP_HASHVAL, digest, &len, 0);
        CryptDestroyHash(h);
    }
    memcpy((void *)(uintptr_t)A0, digest, A1 < sizeof digest ? (size_t)A1 : sizeof digest);
}
/* Md5Impl: the guest object is not touched; the running hash is kept on the host, keyed by its address. */
static struct { uint64_t key; HCRYPTHASH h; } g_md5[64];
static CRITICAL_SECTION g_md5_lock;
static HCRYPTHASH *md5_slot(uint64_t key, int create) {
    HCRYPTHASH *found = NULL;
    EnterCriticalSection(&g_md5_lock);
    for (int i = 0; i < 64 && !found; i++) if (g_md5[i].key == key) found = &g_md5[i].h;
    for (int i = 0; i < 64 && !found && create; i++) if (!g_md5[i].key) { g_md5[i].key = key; g_md5[i].h = 0; found = &g_md5[i].h; }
    LeaveCriticalSection(&g_md5_lock);
    return found;
}
static void md5_drop(uint64_t key) {
    EnterCriticalSection(&g_md5_lock);
    for (int i = 0; i < 64; i++) if (g_md5[i].key == key) { if (g_md5[i].h) CryptDestroyHash(g_md5[i].h); g_md5[i].key = 0; g_md5[i].h = 0; }
    LeaveCriticalSection(&g_md5_lock);
}
static void crypto_Md5_Initialize(Ctx *c) {
    md5_drop(A0);
    HCRYPTHASH *h = md5_slot(A0, 1);
    if (!h) fatal(c, "nn::crypto Md5Impl: more than 64 hashes in progress");
    if (!crypt_provider() || !CryptCreateHash(crypt_provider(), CALG_MD5, 0, 0, h)) *h = 0;
}
static void crypto_Md5_Update(Ctx *c) { HCRYPTHASH *h = md5_slot(A0, 0); if (h && *h) CryptHashData(*h, (const BYTE *)(uintptr_t)A1, (DWORD)A2, 0); }
static void crypto_Md5_GetHash(Ctx *c) {                  /* (this, void *out, size_t outSize) */
    HCRYPTHASH *h = md5_slot(A0, 0);
    uint8_t digest[16] = {0};
    DWORD len = sizeof digest;
    if (h && *h) CryptGetHashParam(*h, HP_HASHVAL, digest, &len, 0);
    memcpy((void *)(uintptr_t)A1, digest, A2 < sizeof digest ? (size_t)A2 : sizeof digest);
}
static void crypto_Md5_dtor(Ctx *c) { md5_drop(A0); }

/* ---------------------------------------------------------------- nn::friends, nn::account: no friends, no network
 * An asynchronous request that has already finished (with a failure): its event is signalled. */
static void async_GetSystemEvent(Ctx *c) { os_init_event(A1, 0); os_signal_event(A1); RET(0); }   /* (this, SystemEvent *out) */

/* nn::nfp, nn::web, nn::swkbd: nnapplet.c. nn::nifm leftovers, nn::nsd, nn::ssl and sockets: nnsocket.inc (included by
 * nnnet.c). */

/* ---------------------------------------------------------------- byte order helpers of nn::socket */
static void socket_swap16(Ctx *c) { RET((uint16_t)(((A0 & 0xff) << 8) | ((A0 >> 8) & 0xff))); }
static void socket_swap32(Ctx *c) { RET(bswap32((uint32_t)A0)); }

/* ---------------------------------------------------------------- registration */
#define OE(n) "_ZN2nn2oe" n
#define PR(n) "_ZN2nn5prepo10PlayReport" n
#define FR(n) "_ZN2nn7friends" n
#define AC(n) "_ZN2nn7account" n
#define SO(n) "_ZN2nn6socket" n

/* Replace whatever an earlier table registered for these names. */
static const ImportDef replace_imports[] = {
    {"_ZN2nn2oe25TryPopNotificationMessageEPj", oe_TryPopNotificationMessage}, {"_ZN2nn2oe20GetCurrentFocusStateEv", oe_GetCurrentFocusState},
    {"_ZN2nn2oe20SetFocusHandlingModeENS0_17FocusHandlingModeE", oe_SetFocusHandlingMode},
    {"_ZN2nn2oe42SetOperationModeChangedNotificationEnabledEb", oe_SetOperationModeChangedNotificationEnabled},
    {"_ZN2nn2oe18GetDesiredLanguageEv", oe_GetDesiredLanguage},
    {"_ZN2nn8settingseqERKNS0_12LanguageCodeERKNS0_8LanguageE", settings_CodeEqualsLanguage},
};

/* Only installed where no other file provides the function. */
static const ImportDef fill_imports[] = {
    /* C wrappers */
    {"nnosGetCurrentThread", nnos_GetCurrentThread}, {"nnosCreateThread", nnos_CreateThread}, {"nnosStartThread", nnos_StartThread},
    {"nnosWaitThread", nnos_WaitThread}, {"nnosDestroyThread", nnos_DestroyThread}, {"nnosGetThreadPriority", nnos_GetThreadPriority},
    {"nnosGetThreadCoreMask", nnos_GetThreadCoreMask}, {"nnosSetThreadCoreMask", nnos_SetThreadCoreMask},
    {"nnosSetThreadNamePointer", nnos_SetThreadName}, {"nnosSleepThread", nnos_SleepThread},
    {"nnosInitializeMutex", nnos_InitializeMutex}, {"nnosFinalizeMutex", nnos_FinalizeMutex}, {"nnosLockMutex", nnos_LockMutex},
    {"nnosUnlockMutex", nnos_UnlockMutex}, {"nnosInitializeEvent", nnos_InitializeEvent}, {"nnosFinalizeEvent", nnos_FinalizeEvent},
    {"nnosSignalEvent", nnos_SignalEvent}, {"nnosWaitEvent", nnos_WaitEvent}, {"nnosGetSystemTick", nnos_GetSystemTick},
    {"nnosGetSystemTickFrequency", nnos_GetSystemTickFrequency}, {"nnosConvertTickToNanoSeconds", nnos_ConvertTickToNanoSeconds},
    {"_ZN2nn4diag6detail9AbortImplEPKcS3_S3_i", diag_AbortImpl}, {"nndetailDiagAbortFormatImpl", diag_AbortFormatImpl},
    {"_ZN2nn6detail21UnexpectedDefaultImplEPKcS2_i", diag_UnexpectedDefault},
    /* nn::oe */
    {OE("25TryPopNotificationMessageEPj"), oe_TryPopNotificationMessage}, {OE("20GetCurrentFocusStateEv"), oe_GetCurrentFocusState},
    {OE("17FinishStartupLogoEv"), keep}, {OE("42SetOperationModeChangedNotificationEnabledEb"), keep},
    {OE("15SetCpuBoostModeENS0_12CpuBoostModeE"), keep},
    /* nn::err */
    {"_ZN2nn3err9ShowErrorENS_6ResultE", err_ShowErrorResult},
    /* nn::prepo */
    {PR("9SetBufferEPvm"), keep},
    {PR("3AddEPKcRKNS0_10Any64BitIdE"), ret0}, {PR("3AddEPKcl"), ret0}, {PR("3AddEPKcd"), ret0},
    {PR("4SaveERKNS_7account3UidE"), ret0},
    /* nn::util, nn::crypto */
    {"_ZNK2nn4util16BinaryFileHeader16IsSignatureValidEl", util_BinaryFileHeader_IsSignatureValid},
    {"_ZNK2nn4util16BinaryFileHeader16IsAlignmentValidEv", util_BinaryFileHeader_IsAlignmentValid},
    {"_ZNK2nn4util16BinaryFileHeader15IsEndianReverseEv", util_BinaryFileHeader_IsEndianReverse},
    {"_ZN2nn6crypto16GenerateSha1HashEPvmPKvm", crypto_Sha1},
    {"_ZN2nn6crypto6detail7Md5Impl10InitializeEv", crypto_Md5_Initialize}, {"_ZN2nn6crypto6detail7Md5Impl6UpdateEPKvm", crypto_Md5_Update},
    {"_ZN2nn6crypto6detail7Md5Impl7GetHashEPvm", crypto_Md5_GetHash}, {"_ZN2nn6crypto6detail7Md5ImplD1Ev", crypto_Md5_dtor},
    /* nn::friends */
    {FR("12AsyncContextD1Ev"), keep}, {FR("12AsyncContext14GetSystemEventEPNS_2os11SystemEventE"), async_GetSystemEvent},
    {"_ZNK2nn7friends12AsyncContext9GetResultEv", unavailable},
    {"_ZNK2nn7friends7Profile7IsValidEv", ret0},
    /* nn::account */
    {AC("44EnsureNetworkServiceAccountIdTokenCacheAsyncEPNS0_12AsyncContextERKNS0_10UserHandleE"), acc_nsa_unavailable},
    {AC("12AsyncContext6CancelEv"), ret0},
    {AC("12AsyncContext14GetSystemEventEPNS_2os11SystemEventE"), async_GetSystemEvent},
    {AC("37LoadNetworkServiceAccountIdTokenCacheEPmPcmRKNS0_10UserHandleE"), acc_nsa_unavailable},
    {AC("26GetNetworkServiceAccountIdEPNS0_23NetworkServiceAccountIdERKNS0_10UserHandleE"), acc_nsa_unavailable},
    /* sockets */
    {SO("9InetHtonsEt"), socket_swap16}, {SO("9InetNtohsEt"), socket_swap16}, {SO("9InetHtonlEj"), socket_swap32}, {SO("9InetNtohlEj"), socket_swap32},
    {"nnsocketInetHtons", socket_swap16}, {"nnsocketInetNtohs", socket_swap16},
};

/* SMO_DUMP_UNREG=<file>: write the names of all imports that still have no handler. */
static void dump_unregistered(void) {
    const char *path = getenv("SMO_DUMP_UNREG");
    if (!path || !path[0]) return;
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (int i = 0; i < dyn_sym_count; i++)
        if (!dyn_syms[i].defined && dyn_syms[i].name[0] && !import_lookup(dyn_syms[i].name)) fprintf(f, "%s\n", dyn_syms[i].name);
    fclose(f);
}

/* self-test: the image's text is read-only, its data writable (hostrt.c runtime_init). */
int image_protect_selftest(Ctx *c) {
    int bad = 0, n = 0;
    (void)c;
    if (!getenv("SMO_NO_PROTECT")) {
        n += 3;
        if (!IsBadWritePtr((void *)(uintptr_t)GUEST_BASE, 1)) { bad++; printf("  image protection: the first text page is writable\n"); }
        if (!IsBadWritePtr((void *)(uintptr_t)(GUEST_BASE + GUEST_RO_END - 1), 1)) { bad++; printf("  image protection: the last read-only data byte is writable\n"); }
        if (IsBadWritePtr((void *)(uintptr_t)(GUEST_BASE + GUEST_RO_END), 1) || IsBadReadPtr((void *)(uintptr_t)GUEST_BASE, 16)) { bad++; printf("  image protection: data segment not writable or text not readable\n"); }
    }
    printf("  image protection: %d cases, %d failed\n", n, bad);
    return bad;
}

void register_misc_imports(void) {
    QueryPerformanceCounter(&g_start);
    InitializeCriticalSection(&g_oe_lock);
    InitializeCriticalSection(&g_md5_lock);
    register_imports(replace_imports, (int)(sizeof replace_imports / sizeof replace_imports[0]));
    int n = (int)(sizeof fill_imports / sizeof fill_imports[0]), added = 0;
    for (int i = 0; i < n; i++) {
        if (!import_data(fill_imports[i].name)) { printf("nnmisc: \"%s\" is not an import of the game (a typo, or a function it never calls)\n", fill_imports[i].name); continue; }
        if (!import_lookup(fill_imports[i].name)) { register_imports(&fill_imports[i], 1); added++; }
    }
    for (int i = 0; i < (int)(sizeof replace_imports / sizeof replace_imports[0]); i++)
        if (!import_data(replace_imports[i].name)) printf("nnmisc: \"%s\" is not an import of the game (a typo, or a function it never calls)\n", replace_imports[i].name);
    trace_printf("nnmisc: %d of %d functions installed (the rest are provided elsewhere or not imported)\n", added, n);
    dump_unregistered();
    (void)ret1;
}
