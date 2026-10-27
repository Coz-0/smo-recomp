/* nnhid.c / nnpad_sdl.c on macOS with SDL3, without the rest of the runtime: their own self-test (SMO_PAD_SELFTEST's:
 * virtual pads through SDL's virtual joystick API, hot-plug, players, remapping file, rumble, motion). The game parts
 * nnhid.c talks to are stand-ins that report nothing. Exit code = failures. */
#include <stdio.h>
#include <stdlib.h>
#include "runtime.h"
#include "hostrt.h"

static volatile int g_menu_asked;                     /* as smo_amiibo.c keeps it: asked, then taken once */
void amiibo_menu_request(void) { g_menu_asked = 1; }
int amiibo_menu_take_request(void) { int r = g_menu_asked; g_menu_asked = 0; return r; }
int display_window_foreground(void) { return 0; }
void guest_trap(const char *what, uint64_t addr) { printf("GUEST TRAP: %s (%llx)\n", what, (unsigned long long)addr); exit(2); }
int nerve_active(const char *name) { (void)name; return 0; }
int nerve_actor_pos(const char *prefix, float pos[3]) { (void)prefix; (void)pos; return 0; }
uint64_t nerve_player_actor(void) { return 0; }
const char *nerve_player_nerve(void) { return ""; }
int nerve_player(float pos[3]) { (void)pos; return 0; }
void nerve_release(void) {}
int nerve_steer(float tx, float tz, float r, float *sx, float *sy, float *dist) { (void)tx; (void)tz; (void)r; (void)sx; (void)sy; (void)dist; return 0; }
int nerve_warp_state(void) { return 0; }
void nerve_warp(float x, float y, float z) { (void)x; (void)y; (void)z; }
long long nerve_watch_last(void) { return 0; }
long nfp_amiibo_reads(void) { return 0; }
long long nvn_front_frame_next(void) { return 0; }
void nvn_front_shot_step(int step) { (void)step; }
void register_imports(const ImportDef *defs, int count) { (void)defs; (void)count; }
void report_and_exit(int code) { exit(code); }
void sdk_host_override(const char *name, ImportFn fn) { (void)name; (void)fn; }
void smo_capture_request(void) {}
const char *stage_list_begun(void) { return NULL; }
int stage_list_has_next(void) { return 0; }
void stage_list_seq_done(long long poll) { (void)poll; }
int stage_list_waiting(void) { return 0; }
int nnhid_pad_selftest(void);

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    return nnhid_pad_selftest() != 0;
}
