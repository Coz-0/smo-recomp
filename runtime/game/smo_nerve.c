/* smo_nerve.c - test aid: which state (nerve) each of the game's state machines is in.
 *
 * A state of an al::NerveKeeper is a const al::Nerve; al::NerveKeeper::update calls its execute(NerveKeeper *) once per
 * frame through the vtable, so through call_indirect (hostrt.c). The function list (tools/extract/functions.py -> build\functions\
 * main_functions.tsv) names such execute functions by the class RTTI gives (the game's main has almost no symbols;
 * in the base game's main, which has them, RTTI's names equal the symbols wherever both exist). The others are named
 * here (nerve_load): from the <Class>::exe<State> function their code branches to (FrogNrvWait), for an execute that
 * runs a sub-state from the nerves of its class around it (FrogNrv_38d710), and a HostTypeNrv<State> class (the game's
 * own name, the same in many host classes) by the class of the object that first runs it. When one of them is called,
 * the frame is noted for that nerve and it becomes the keeper's current nerve.
 *
 *   SMO_NERVELOG=<s>[,<s>...]   print "nerve: frame F keeper K: A -> B" when a keeper's nerve changes and A or B contains
 *                               one of the substrings ("*" = every keeper)
 *   SMO_ACTORPOS=<n>[:<p>,...]  every n frames: the position of each actor whose nerve name starts with one of the
 *                               prefixes (default PlayerActorHakoniwaNrv, the player): the keeper's parent (NerveKeeper +0)
 *                               taken as the al::LiveActor, al::getTrans = pose keeper at +0x50, translation at +8. A
 *                               keeper whose parent is not an actor (a nerve state object) gives no line: reads that
 *                               fault are caught
 *   SMO_SEQ step @nerve:<name>  (nnhid.c) waits until the nerve <name> ran in this frame or the one before
 *   SMO_SEQ step @goto:<x>,<z>  (nnhid.c) steers the player to a world point with the camera read here (nerve_steer);
 *                               @near / @chase steer to an actor found by its nerve (nerve_actor_pos), @at waits for the
 *                               player at a point (nerve_player)
 *   SMO_FUNCTIONS=<path>        the function list (default ..\functions\main_functions.tsv, relative to build\bin)
 *   SMO_START_STAGE=<stage>[:<scenario>]  TEST ONLY: the first stage scene starts that stage (see start_stage_init); it
 *                               writes game memory, as does the sequence step @warp (player_warp)
 *   SMO_STAGE_LIST=<stage>[:<scenario>[:<start>]],...  TEST ONLY: one stage after another in one run, each changed to by
 *                               the game's own stage change when the input sequence is done (see stage_play_hit)
 *
 * A nerve's name is its mangled nested name without the anonymous namespace, joined with "::"
 * (_ZNK12_GLOBAL__N_135OpeningStageStartDemoNrvWaitStandUp7executeEPN2al11NerveKeeperE -> OpeningStageStartDemoNrvWaitStandUp).
 * Reads game memory only, except for SMO_START_STAGE's one change of a scene's init info, SMO_STAGE_LIST's calls of
 * the game's own stage change and @warp's of the game's own position setters. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include "win32_posix.h"
/* every path of the __try / __except probes below returns; the one-pass loop of the macOS __try hides that */
#pragma clang diagnostic ignored "-Wreturn-type"
#endif
#include "hostrt.h"

long long nvn_front_frame_next(void);                     /* nvn_gl.c: the number the frame being made will get */

#define NRV_MAX 16384
#define NRV_HASH 32768
#define KEEP_HASH 65536
static struct { uint64_t addr; char *volatile name; volatile LONG64 last_frame; char log_ok, pos_ok, host; } g_nrv[NRV_MAX];   /* host: see host_name */
static int g_nrv_count;
static int g_nrv_hash[NRV_HASH];                          /* index + 1 into g_nrv, 0 = empty */
static struct { uint64_t keeper; int nrv; long long pos_frame, last_frame; } g_keep[KEEP_HASH];
static int g_keep_used, g_keep_clears;
static volatile LONG g_nerve_lock;
static char g_filter[512], g_pos_prefix[512] = "PlayerActorHakoniwaNrv";
static int g_filter_all, g_pos_every;
static long long g_transitions, g_logged;
static float g_player_pos[3];
static int g_player_nrv = -1;
static uint64_t g_player_actor;
int g_nerve_mode = -1;                                    /* hostrt.c call_indirect: -1 = not decided yet, 0 = off, 1 = on */

static unsigned hash_addr(uint64_t a, unsigned n) { return (unsigned)(((a >> 2) * 0x9E3779B97F4A7C15ULL) >> 40) % n; }

/* "_ZNK12_GLOBAL__N_135OpeningStageStartDemoNrvWaitStandUp7executeEPN2al11NerveKeeperE" -> "OpeningStageStartDemoNrvWaitStandUp";
 * nested classes are joined with "::". Returns 0 if the name is not a nerve's execute function. */
static int nerve_name(const char *m, char *out, size_t n) {
    static const char tail[] = "7executeEPN2al11NerveKeeperE", tail_al[] = "7executeEPNS_11NerveKeeperE";   /* a class in namespace al: `al` is S_ */
    size_t len = strlen(m), tl = sizeof tail - 1;
    if (len > sizeof tail_al - 1 + 4 && !strcmp(m + len - (sizeof tail_al - 1), tail_al)) tl = sizeof tail_al - 1;
    else if (len <= tl + 4 || strcmp(m + len - tl, tail)) return 0;
    if (strncmp(m, "_ZNK", 4)) return 0;
    const char *p = m + 4, *end = m + len - tl;
    size_t o = 0;
    out[0] = 0;
    while (p < end) {
        if (*p < '0' || *p > '9') return 0;              /* templates and other forms: not handled (none seen in SMO) */
        long k = strtol(p, (char **)&p, 10);
        if (k <= 0 || p + k > end) return 0;
        if (!(k == 12 && !strncmp(p, "_GLOBAL__N_1", 12))) {
            if (o && o + 2 < n) { out[o++] = ':'; out[o++] = ':'; }
            for (long i = 0; i < k && o + 1 < n; i++) out[o++] = p[i];
            out[o] = 0;
        }
        p += k;
    }
    return o > 0;
}

/* "_ZN4Frog7exeWaitEv" -> "FrogNrvWait": the name NERVE_IMPL(Frog, Wait) gives the nerve whose execute calls Frog::exeWait.
 * Returns 0 if the name is not a member function exe<State>() without arguments. */
static int exe_name(const char *m, char *out, size_t n) {
    size_t len = strlen(m);
    if (len < 8 || strncmp(m, "_ZN", 3) || strcmp(m + len - 2, "Ev")) return 0;
    const char *p = m + 3, *end = m + len - 2, *last = NULL;
    long last_len = 0;
    size_t o = 0;
    out[0] = 0;
    while (p < end) {
        if (*p < '0' || *p > '9') return 0;
        long k = strtol(p, (char **)&p, 10);
        if (k <= 0 || p + k > end) return 0;
        if (p + k == end) { last = p; last_len = k; break; }
        if (!(k == 12 && !strncmp(p, "_GLOBAL__N_1", 12))) {
            if (o && o + 2 < n) { out[o++] = ':'; out[o++] = ':'; }
            for (long i = 0; i < k && o + 1 < n; i++) out[o++] = p[i];
            out[o] = 0;
        }
        p += k;
    }
    if (!last || last_len <= 3 || strncmp(last, "exe", 3) || !o) return 0;
    snprintf(out + o, n - o, "Nrv%.*s", (int)(last_len - 3), last + 3);
    return 1;
}

static int filter_match(const char *name);
static int pos_match(const char *name);
static int nrv_index(uint64_t addr);

static void nerve_add(uint64_t a, const char *nm) {
    if (g_nrv_count >= NRV_MAX) return;
    g_nrv[g_nrv_count].addr = a;
    g_nrv[g_nrv_count].name = _strdup(nm);
    g_nrv[g_nrv_count].last_frame = -10;
    g_nrv[g_nrv_count].log_ok = (char)filter_match(nm);             /* the filters are set before the list is loaded */
    g_nrv[g_nrv_count].pos_ok = (char)(g_pos_every > 0 && pos_match(nm));
    unsigned h = hash_addr(a, NRV_HASH);
    while (g_nrv_hash[h]) h = (h + 1) % NRV_HASH;
    g_nrv_hash[h] = ++g_nrv_count;
}

typedef struct { uint64_t addr; char *name; } ExeFn;
static int exe_cmp(const void *a, const void *b) { uint64_t x = ((const ExeFn *)a)->addr, y = ((const ExeFn *)b)->addr; return x < y ? -1 : x > y; }

/* Nerves come from the function list by their names: the symbol (column 6), else the name RTTI gives (column 7,
 * tools/extract/functions.py: almost the only one, as the game's main has almost no symbols). Many classes are
 * called HostTypeNrv<State> by the game itself (a nerve macro of each host class, in its own anonymous namespace):
 * those names say nothing. A small unnamed function, or one with such a name, that branches (B or BL) to
 * <Class>::exe<State>() is taken as that class's nerve and named <Class>Nrv<State>, from its code in the loaded image;
 * a HostType nerve still unnamed then is named when it first runs, from the class of the object that runs it
 * (host_name). */
static void nerve_load(void) {
    const char *path = getenv("SMO_FUNCTIONS");
    FILE *f = fopen(path ? path : "..\\functions\\main_functions.tsv", "r");
    if (!f) { printf("nerve: cannot open the function list %s (set SMO_FUNCTIONS); nerve tracking off\n", path ? path : "..\\functions\\main_functions.tsv"); return; }
    char line[4096], nm[256];
    ExeFn *exe = NULL, *host = NULL;
    uint64_t (*cand)[2] = NULL;                           /* unnamed functions of at most 0x80 bytes: start, end */
    int nexe = 0, cexe = 0, ncand = 0, ccand = 0, named = 0, derived = 0, from_rtti = 0, nhost = 0, chost = 0, have_rtti = 0;
    while (fgets(line, sizeof line, f)) {
        char *col[7];                                     /* start end size src end_from symbol rtti_name */
        int nc = 0;
        for (char *s = line; nc < 7; ) {
            col[nc++] = s;
            char *t = strchr(s, '\t');
            if (!t) { s[strcspn(s, "\r\n")] = 0; break; }
            *t = 0;
            s = t + 1;
        }
        if (nc < 6 || !strcmp(col[0], "start")) continue;
        uint64_t a = GUEST_BASE + strtoull(col[0], NULL, 16), e = GUEST_BASE + strtoull(col[1], NULL, 16);
        char *mangled = col[5];
        mangled[strcspn(mangled, "|")] = 0;
        int rtti_other = 0;                               /* an RTTI name that is not a state (a class's own executeOnEnd) */
        if (nc > 6 && col[6][0]) have_rtti = 1;
        if (nerve_name(mangled, nm, sizeof nm)) { nerve_add(a, nm); named++; continue; }
        if (nc > 6 && col[6][0]) {
            if (nerve_name(col[6], nm, sizeof nm)) {
                if (strncmp(nm, "HostTypeNrv", 11)) { nerve_add(a, nm); from_rtti++; continue; }
                if (nhost == chost) { chost = chost ? chost * 2 : 1024; host = realloc(host, (size_t)chost * sizeof *host); }
                host[nhost].addr = a; host[nhost].name = _strdup(nm); nhost++;
            } else rtti_other = 1;
        }
        if (!mangled[0] && !rtti_other && e > a && e - a <= 0x80) {
            if (ncand == ccand) { ccand = ccand ? ccand * 2 : 4096; cand = realloc(cand, (size_t)ccand * sizeof *cand); }
            cand[ncand][0] = a; cand[ncand][1] = e; ncand++;
        } else if (exe_name(mangled, nm, sizeof nm)) {
            if (nexe == cexe) { cexe = cexe ? cexe * 2 : 4096; exe = realloc(exe, (size_t)cexe * sizeof *exe); }
            exe[nexe].addr = a; exe[nexe].name = _strdup(nm); nexe++;
        }
    }
    fclose(f);
    if (nexe) qsort(exe, (size_t)nexe, sizeof *exe, exe_cmp);
    for (int i = 0; i < ncand; i++) {
        for (uint64_t pc = cand[i][0]; pc < cand[i][1]; pc += 4) {
            uint32_t insn = rd32(pc);
            if ((insn & 0x7C000000) != 0x14000000) continue;              /* B / BL imm26 */
            ExeFn key = { pc + (uint64_t)((int64_t)((int32_t)(insn << 6) >> 6) * 4), NULL };
            ExeFn *hit = nexe ? bsearch(&key, exe, (size_t)nexe, sizeof *exe, exe_cmp) : NULL;
            if (hit && nrv_index(cand[i][0]) < 0) { nerve_add(cand[i][0], hit->name); derived++; }
            break;                                       /* only the first branch counts */
        }
    }
    for (int i = 0; i < nexe; i++) free(exe[i].name);
    free(exe);
    int hosted = 0;
    for (int i = 0; i < nhost; i++) {                    /* HostType nerves the exe functions did not name: named when they run */
        if (nrv_index(host[i].addr) < 0) { nerve_add(host[i].addr, host[i].name); g_nrv[g_nrv_count - 1].host = 1; hosted++; }
        free(host[i].name);
    }
    free(host);
    /* Nerves whose execute runs a sub-state (al::updateNerveState / updateNerveStateAndNextNerve) call no exe function:
     * an unnamed candidate that loads the keeper's parent (ldr xN, [x1] in its first six instructions) and lies between
     * two nerves of the same class (the compiler keeps a class's nerves together; neighbours named with the function
     * list's placeholder class HostType are skipped) is named <Class>Nrv_<address>. Not when the list has RTTI names:
     * they name every nerve class, so what is left unnamed is not a nerve (80 such guesses). */
    int nall = have_rtti ? 0 : g_nrv_count, block = 0;
    ExeFn *all = malloc(sizeof *all * (size_t)(nall ? nall : 1));
    for (int i = 0; i < nall; i++) { all[i].addr = g_nrv[i].addr; all[i].name = g_nrv[i].name; }
    qsort(all, (size_t)nall, sizeof *all, exe_cmp);
    for (int i = 0; i < ncand; i++) {
        uint64_t a = cand[i][0];
        int loads = 0;
        if (nrv_index(a) >= 0) continue;
        for (int k = 0; k < 6 && a + 4u * k < cand[i][1]; k++) if ((rd32(a + 4u * k) & 0xFFFFFFE0u) == 0xF9400020u) { loads = 1; break; }
        if (!loads) continue;
        int lo = 0, hi = nall;                             /* first entry above a */
        while (lo < hi) { int mid = (lo + hi) / 2; if (all[mid].addr <= a) lo = mid + 1; else hi = mid; }
        const char *before = NULL, *after = NULL;
        for (int k = lo - 1; k >= 0 && a - all[k].addr <= 0x200; k--) if (strncmp(all[k].name, "HostTypeNrv", 11)) { before = all[k].name; break; }
        for (int k = lo; k < nall && all[k].addr - a <= 0x200; k++) if (strncmp(all[k].name, "HostTypeNrv", 11)) { after = all[k].name; break; }
        const char *nb = before ? strstr(before, "Nrv") : NULL, *na = after ? strstr(after, "Nrv") : NULL;
        if (!nb || !na || nb - before != na - after || strncmp(before, after, (size_t)(nb - before))) continue;
        snprintf(nm, sizeof nm, "%.*sNrv_%llx", (int)(nb - before), before, (unsigned long long)(a - GUEST_BASE));
        nerve_add(a, nm);
        block++;
    }
    free(all);
    free(cand);
    printf("nerve: %d nerve states: %d named in the function list, %d from RTTI, %d named here from the exe function they call, "
           "%d from their class's block, %d HostType ones named by the class of the object that first runs them\n",
           g_nrv_count, named, from_rtti, derived, block, hosted);
}

static void start_stage_init(void);
static void stage_list_init(void);
static void nerve_init(void) {                           /* first indirect call (any thread); the others wait for it */
    static volatile LONG once;
    if (InterlockedCompareExchange(&once, 1, 0)) { while (*(volatile int *)&g_nerve_mode < 0) YieldProcessor(); return; }
    start_stage_init();
    const char *log = getenv("SMO_NERVELOG"), *pos = getenv("SMO_ACTORPOS"), *seq = getenv("SMO_SEQ");
    int want = (log && *log) || (pos && atoi(pos) > 0) || (seq && (strstr(seq, "@nerve:") || strstr(seq, "@until-nerve:") || strstr(seq, "@goto:") ||
                                                                    strstr(seq, "@near:") || strstr(seq, "@at:") || strstr(seq, "@chase:") ||
                                                                    strstr(seq, "@warp:")));
    if (log) { snprintf(g_filter, sizeof g_filter, "%s", log); g_filter_all = !strcmp(log, "*"); }
    g_pos_every = pos ? atoi(pos) : 0;
    if (pos && strchr(pos, ':')) snprintf(g_pos_prefix, sizeof g_pos_prefix, "%s", strchr(pos, ':') + 1);
    if (want) nerve_load();
    stage_list_init();
    MemoryBarrier();
    g_nerve_mode = want && g_nrv_count > 0;
}

static int nrv_index(uint64_t addr) {
    unsigned h = hash_addr(addr, NRV_HASH);
    for (int probe = 0; probe < NRV_HASH && g_nrv_hash[h]; probe++, h = (h + 1) % NRV_HASH)
        if (g_nrv[g_nrv_hash[h] - 1].addr == addr) return g_nrv_hash[h] - 1;
    return -1;
}

static int pos_match(const char *name) {
    char buf[512];
    snprintf(buf, sizeof buf, "%s", g_pos_prefix);
    for (char *s = strtok(buf, ","); s; s = strtok(NULL, ",")) if (*s && !strncmp(name, s, strlen(s))) return 1;
    return 0;
}

/* al::getTrans of the keeper's parent, if the parent is an actor; 0 if a pointer on the way is null or unreadable */
static int actor_trans(uint64_t keeper, float out[3]) {
    __try {
        uint64_t actor = rd64(keeper), pose = actor ? rd64(actor + 0x50) : 0;
        if (!pose) return 0;
        memcpy(out, (void *)(uintptr_t)(pose + 8), 12);
        return out[0] == out[0] && out[1] == out[1] && out[2] == out[2];
    } __except (1) { return 0; }
}

/* The class of a game object from its RTTI: vtable -> typeinfo (vtable - 8) -> the type's mangled name ("4Frog",
 * "N12_GLOBAL__N_17FooBarE", ...) -> its last component. 0 if a read faults or the name has another form (templates). */
static int class_of(uint64_t obj, char *out, size_t n) {
    __try {
        uint64_t vt = rd64(obj), ti = vt ? rd64(vt - 8) : 0, nm = ti ? rd64(ti + 8) : 0;
        const char *p = (const char *)(uintptr_t)nm, *last = NULL;
        long last_len = 0;
        if (!p) return 0;
        int nested = *p == 'N';
        p += nested;
        while (*p >= '1' && *p <= '9') {
            char *q;
            long k = strtol(p, &q, 10);
            if (k <= 0 || k > 200) return 0;
            last = q; last_len = k; p = q + k;
            if (!nested) break;
        }
        if (!last || (nested && *p != 'E') || (size_t)last_len + 1 > n) return 0;
        memcpy(out, last, (size_t)last_len);
        out[last_len] = 0;
        return 1;
    } __except (1) { return 0; }
}

/* A HostType nerve (host = 1) gets its real name the first time it runs: HostTypeNrv<State> -> <Class>Nrv<State>, the
 * class being that of the keeper's parent (NerveKeeper +0: the object whose exe<State> the nerve calls). host = 2 when
 * named, 3 when the class could not be read (the name stays). Called with g_nerve_lock held. */
static void host_name(int i, uint64_t keeper) {
    char cls[200], nm[256];
    uint64_t parent = 0;
    __try { parent = rd64(keeper); } __except (1) { parent = 0; }
    if (!parent || !class_of(parent, cls, sizeof cls)) { g_nrv[i].host = 3; return; }
    snprintf(nm, sizeof nm, "%sNrv%s", cls, g_nrv[i].name + 11);
    g_nrv[i].log_ok = (char)filter_match(nm);
    g_nrv[i].pos_ok = (char)(g_pos_every > 0 && pos_match(nm));
    g_nrv[i].name = _strdup(nm);                          /* the old name stays allocated: another thread may be reading it */
    g_nrv[i].host = 2;
}

static int filter_match(const char *name) {
    if (g_filter_all) return 1;
    if (!g_filter[0] || !name) return 0;
    char buf[512];
    snprintf(buf, sizeof buf, "%s", g_filter);
    for (char *s = strtok(buf, ","); s; s = strtok(NULL, ",")) if (*s && strstr(name, s)) return 1;
    return 0;
}

/* SMO_SEQ "@warp:<x>,<y>,<z>" (nnhid.c), TEST ONLY: the player is moved to the point by the game's own
 * al::updatePoseTrans (as PlayerPuppet::setTrans moves a player another actor controls) and al::setVelocityZero, called
 * when his state machine next runs (nerve_call, on that thread, with its registers put back after). A shortcut for a
 * test to reach a place whose way there is not scripted; it writes game state. */
static volatile LONG g_warp_state;                        /* 0 none, 1 asked, 2 done, 3 cannot (not in this build) */
static __declspec(align(16)) float g_warp_to[4];
void nerve_warp(float x, float y, float z) { g_warp_to[0] = x; g_warp_to[1] = y; g_warp_to[2] = z; InterlockedExchange(&g_warp_state, 1); }
int nerve_warp_state(void) { return g_warp_state; }
static void player_warp(Ctx *c, uint64_t actor) {
    if (!GAME_FN_updatePoseTrans || !GAME_FN_setVelocityZero) { InterlockedExchange(&g_warp_state, 3); return; }
    Ctx saved = *c;
    c->x[0] = actor;
    c->x[1] = (uint64_t)(uintptr_t)g_warp_to;
    call_guest(c, GAME_FN_updatePoseTrans);
    c->x[0] = actor;
    call_guest(c, GAME_FN_setVelocityZero);
    *c = saved;
    printf("nerve: frame %lld: player moved from %.1f %.1f %.1f to %.1f %.1f %.1f (@warp: al::updatePoseTrans)\n", nvn_front_frame_next(),
           g_player_pos[0], g_player_pos[1], g_player_pos[2], g_warp_to[0], g_warp_to[1], g_warp_to[2]);
    fflush(stdout);
    InterlockedExchange(&g_warp_state, 2);
}

void nerve_call(Ctx *c, uint64_t addr) {
    if (g_nerve_mode < 0) nerve_init();
    if (g_nerve_mode <= 0) return;
    int i = nrv_index(addr);
    if (i < 0) return;
    long long frame = nvn_front_frame_next();
    uint64_t keeper = c->x[1];
    g_nrv[i].last_frame = frame;
    while (InterlockedCompareExchange(&g_nerve_lock, 1, 0)) YieldProcessor();
    if (g_keep_used > KEEP_HASH * 3 / 4) { memset(g_keep, 0, sizeof g_keep); g_keep_used = 0; g_keep_clears++; }
    unsigned h = hash_addr(keeper, KEEP_HASH);
    while (g_keep[h].keeper && g_keep[h].keeper != keeper) h = (h + 1) % KEEP_HASH;
    int old = g_keep[h].keeper ? g_keep[h].nrv : -1;
    if (g_nrv[i].host == 1) host_name(i, keeper);
    if (!g_keep[h].keeper) { g_keep[h].keeper = keeper; g_keep[h].pos_frame = -1; g_keep_used++; }
    g_keep[h].nrv = i;
    g_keep[h].last_frame = frame;
    if (old != i) {
        g_transitions++;
        if (g_nrv[i].log_ok || (old >= 0 && g_nrv[old].log_ok)) {
            g_logged++;
            printf("nerve: frame %lld keeper %llx: %s -> %s\n", frame, (unsigned long long)keeper, old >= 0 ? g_nrv[old].name : "(new)", g_nrv[i].name);
        }
    }
    int warp = 0;
    if (!strncmp(g_nrv[i].name, "PlayerActorHakoniwaNrv", 22) && actor_trans(keeper, g_player_pos)) {
        g_player_nrv = i;
        g_player_actor = rd64(keeper);
        warp = g_warp_state == 1;
    }
    if (g_nrv[i].pos_ok && (g_keep[h].pos_frame < 0 || frame - g_keep[h].pos_frame >= g_pos_every)) {
        float p[3];
        if (actor_trans(keeper, p)) {
            g_keep[h].pos_frame = frame;
            printf("actor: frame %lld keeper %llx pos %.1f %.1f %.1f nerve %s\n", frame, (unsigned long long)keeper, p[0], p[1], p[2], g_nrv[i].name);
        }
    }
    g_nerve_lock = 0;
    if (warp) player_warp(c, g_player_actor);             /* the game's calls run outside the lock */
}

/* the amiibo menu (smo_amiibo.c, nnhid.c): two nerve states watched without the tracker. hostrt.c call_indirect
 * compares each indirect call with their execute functions (two compares while a watch is set, none before) and the frame
 * either last ran is kept. The names are resolved with the tracker's list (loaded here once if the tracker is off). */
volatile uint64_t g_nerve_watch[2];
static volatile LONG64 g_watch_frame = -1;
void nerve_watch_hit(void) { g_watch_frame = nvn_front_frame_next(); }
long long nerve_watch_last(void) { return g_watch_frame; }
int nerve_watch_set(const char *a, const char *b) {
    static int loaded;
    if (g_nerve_mode < 0) nerve_init();
    if (g_nrv_count == 0 && !loaded) { loaded = 1; nerve_load(); }
    uint64_t found[2] = {0, 0};
    for (int i = 0; i < g_nrv_count; i++) {
        if (!strcmp(g_nrv[i].name, a)) found[0] = g_nrv[i].addr;
        if (b && !strcmp(g_nrv[i].name, b)) found[1] = g_nrv[i].addr;
    }
    static int said;
    if (!said++) printf("nerve: watching %s (%s) and %s (%s)\n", a, found[0] ? "found" : "NOT found", b ? b : "-", found[1] ? "found" : "NOT found");
    if (!found[0]) found[0] = found[1];                   /* call_indirect tests the first one first */
    if (!found[1]) found[1] = found[0];
    g_nerve_watch[1] = found[1];
    g_nerve_watch[0] = found[0];
    return found[0] != 0;
}

/* TEST ONLY, SMO_START_STAGE=<stage>[:<scenario>]: the first stage scene the game starts is that stage (in that
 * scenario) instead of the one it chose. A save always resumes in a kingdom's home stage; a test starts any other stage
 * this way, as the game does when Mario goes through the stage's entrance, but at the stage's default start point.
 * StageScene::init(const al::SceneInitInfo &) runs through the scene's vtable (GAME_VSYM_StageScene_init, gen_tables.py):
 * call_indirect (hostrt.c) compares each indirect call with it; the first call gets the init info's stage name (+0x18)
 * replaced and its scenario (+0x20): the one given, else the stage's own scenario when the game has one for it (what
 * its door gives, use_scenario: 1 for a kingdom's challenge rooms), else the kingdom's as the game chose it. Later calls
 * (the game's own, e.g. the restart after a miss) are only logged. The game then starts that stage itself
 * (GameDataHolder::startStage from StageScene::init). */
volatile uint64_t g_stage_init;                           /* hostrt.c call_indirect: the function watched for, 0 = none */
static char g_start_stage[128];
static int g_start_scenario = -1;
static int g_stage_inits;                                 /* StageScene::init calls seen */

/* TEST ONLY, SMO_STAGE_LIST=<stage>[:<scenario>[:<start>]],...: several stages one after another in one run, so that a
 * test of many stages boots the game once (tests/stress_stages.py --chain). The first entry is the stage the run starts
 * in (the save's, or SMO_START_STAGE's). Whenever the input sequence is done (nnhid.c), the game changes to the next
 * entry through its own stage change, the one a door makes: at the scene's next StageSceneNrvPlay (nnhid.c presses B
 * every 45 polls while the change waits for it: a map, the pause menu or a message left open) a ChangeStageInfo
 * (GameDataHolder, start, stage, not a return, scenario, no sub-scenario) is made by the game's own constructor and given
 * to GameDataFunction::tryChangeNextStage (GAME_FN_*, gen_tables.py); StageScene::exePlay then ends the scene and the
 * game's sequence loads the stage, in a world of its own when it is a kingdom's home stage. <scenario> (default -1: the
 * game's choice, as for a door) and <start>, the id of a start point (e.g. a checkpoint flag's PlayerRestartPos; default
 * the stage's default start), are optional. The GameDataHolder is the one the stage scenes are made with (SceneInitInfo
 * +8, checked by its RTTI). When the next StageScene::init runs, the input sequence starts again: SMO_SEQ_<n> for the
 * n-th entry (1-based), else SMO_STAGE_SEQ, else SMO_SEQ. Lines: "stage list: frame F: stage n of N begins: the game
 * starts <stage>, scenario <s>" at each StageScene::init of the list, "stage list: stage n of N done at frame F ...: run
 * totals: ..." when its input sequence is done (the counters the end-of-run summary prints, so far, for tests to take
 * apart per stage); after the last entry the run ends as with SMO_SEQ_EXIT=1. boot.c: the run's time limit counts from
 * the start of the entry being played (the moment its stage change was asked for). */
#define LIST_MAX 64
static struct { char stage[96], start[160]; int scenario; } g_list[LIST_MAX];
static int g_list_n;                                      /* entries; 0 = no stage list */
static volatile LONG g_list_cur;                          /* the entry being played, 0-based */
enum { LIST_PLAY, LIST_ASKED, LIST_CHANGING, LIST_BEGUN };
static volatile LONG g_list_state;                        /* LIST_ASKED: waits for StageSceneNrvPlay; LIST_CHANGING: for the next StageScene::init */
static uint64_t g_holder;                                 /* the game's GameDataHolder */
static uint64_t g_play_exec;                              /* StageSceneNrvPlay's execute */
volatile uint64_t g_stage_play;                           /* hostrt.c call_indirect: g_play_exec while a change waits for it, else 0 */
static volatile ULONGLONG g_list_tick;                    /* see stage_list_started */

static void start_stage_init(void) {                      /* nerve_init: before the first indirect call goes on */
    const char *e = getenv("SMO_START_STAGE"), *l = getenv("SMO_STAGE_LIST");
    if (e && e[0]) {
        snprintf(g_start_stage, sizeof g_start_stage, "%s", e);
        char *colon = strchr(g_start_stage, ':');
        if (colon) { *colon = 0; g_start_scenario = atoi(colon + 1); }
        if (!GAME_VSYM_StageScene_init) printf("start stage: SMO_START_STAGE is set, but this build does not know StageScene::init (gen_tables.py): ignored\n");
        else {
            printf("start stage: the first stage scene will start %s (SMO_START_STAGE=%s, TEST ONLY)\n", g_start_stage, e);
            g_stage_init = GAME_VSYM_StageScene_init;
        }
    }
    if (!l || !l[0]) return;
    if (!GAME_VSYM_StageScene_init || !GAME_FN_ChangeStageInfo_ctor || !GAME_FN_tryChangeNextStage) {
        printf("stage list: SMO_STAGE_LIST is set, but this build does not know StageScene::init, ChangeStageInfo's constructor and "
               "GameDataFunction::tryChangeNextStage (gen_tables.py): ignored\n");
        return;
    }
    for (const char *s = l; *s && g_list_n < LIST_MAX; ) {
        size_t len = strcspn(s, ",");
        char tok[300];
        snprintf(tok, sizeof tok, "%.*s", (int)(len < sizeof tok ? len : sizeof tok - 1), s);
        s += len + (s[len] == ',');
        if (!tok[0]) continue;
        char *f2 = strchr(tok, ':'), *f3 = f2 ? strchr(f2 + 1, ':') : NULL;
        if (f2) *f2++ = 0;
        if (f3) *f3++ = 0;
        snprintf(g_list[g_list_n].stage, sizeof g_list[0].stage, "%s", tok);
        g_list[g_list_n].scenario = f2 && *f2 ? atoi(f2) : -1;
        snprintf(g_list[g_list_n].start, sizeof g_list[0].start, "%s", f3 ? f3 : "");
        g_list_n++;
    }
    printf("stage list: %d stages in one run (SMO_STAGE_LIST, TEST ONLY):", g_list_n);
    for (int i = 0; i < g_list_n; i++) printf(" %s%s", g_list[i].stage, i + 1 < g_list_n ? "," : "\n");
    g_stage_init = GAME_VSYM_StageScene_init;             /* every StageScene::init, while the list lasts */
}
static void stage_list_init(void) {                       /* nerve_init, after the nerve list may have been loaded */
    if (!g_list_n) return;
    if (!g_nrv_count) nerve_load();                       /* for the address of StageSceneNrvPlay's execute; tracking stays as decided */
    for (int i = 0; i < g_nrv_count && !g_play_exec; i++) if (!strcmp(g_nrv[i].name, "StageSceneNrvPlay")) g_play_exec = g_nrv[i].addr;
    if (!g_play_exec) { printf("stage list: the nerve StageSceneNrvPlay is not in the function list: no stage after the first\n"); g_list_n = 1; }
}

/* The counters of the end-of-run summary so far, in one line (stage_list_seq_done); each of them only grows. */
long long display_frames(void);                                                   /* display.c */
void nvn_vk_counts(long long *failures, long long *missing);                      /* nvn_vk.c */
int import_counts(int *stubs);                                                    /* hostrt.c */
void smo_audio_counts(long long *frames, long long *dropped, long long *underruns);   /* smo_audio.c */
void nvn_vk_hold_counts(long long *holds, long long *hold_ms, long long *dummies); /* nvn_vk_pipelines.c */
static void stage_totals(char *out, size_t n) {
    long long fail = 0, miss = 0, af = 0, drop = 0, under = 0, holds = 0, hold_ms = 0, dummies = 0;
    int stubs = 0, imports = import_counts(&stubs);
    nvn_vk_counts(&fail, &miss);
    smo_audio_counts(&af, &drop, &under);
    nvn_vk_hold_counts(&holds, &hold_ms, &dummies);
    PROCESS_MEMORY_COUNTERS_EX pm;
    memset(&pm, 0, sizeof pm);
    K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pm, sizeof pm);
    FILETIME made, ex, k, u, now;                         /* the run's time: since the process was made */
    GetProcessTimes(GetCurrentProcess(), &made, &ex, &k, &u);
    GetSystemTimeAsFileTime(&now);
    double run_s = (double)((((ULONGLONG)now.dwHighDateTime << 32) | now.dwLowDateTime) - (((ULONGLONG)made.dwHighDateTime << 32) | made.dwLowDateTime)) / 1e7;
    snprintf(out, n, "%lld frames presented, %lld Vulkan call failures, %lld calls of operations that are not implemented, imports reached %d "
             "(still stubs %d), audio device sink %lld frames, %lld samples dropped, %lld underruns, loading-screen holds %lld (%lld ms), "
             "samplers given a dummy %lld, process memory %llu MB, run time %.1f s", display_frames(), fail, miss, imports, stubs, af, drop, under,
             holds, hold_ms, dummies, (unsigned long long)(pm.PrivateUsage >> 20), run_s);
}

/* nnhid.c: when the input sequence of the entry being played is done, does a next entry follow? Else the sequence's own
 * end applies (SMO_SEQ_EXIT). */
int stage_list_has_next(void) { return g_list_n && g_list_cur + 1 < g_list_n && g_holder && g_list_state == LIST_PLAY; }

/* nnhid.c: the input sequence of the entry being played is done: its line with the run's totals, and the change to the
 * next entry is asked for */
void stage_list_seq_done(long long poll) {
    if (!g_list_n) return;
    char totals[600];
    stage_totals(totals, sizeof totals);
    int k = g_list_cur;
    long long frame = nvn_front_frame_next();
    printf("stage list: stage %d of %d done at frame %lld (poll %lld): run totals: %s\n", k + 1, g_list_n, frame, poll, totals);
    if (k + 1 < g_list_n && !g_holder) printf("stage list: no stage scene was seen, so no GameDataHolder to change stage with: no stage after this\n");
    if (!stage_list_has_next()) { fflush(stdout); return; }
    unsigned nvn_gl_held_ms(void);                        /* nvn_gl.c: held presents are not run time (boot.c) */
    g_list_tick = GetTickCount64() - nvn_gl_held_ms();
    InterlockedExchange(&g_list_state, LIST_ASKED);
    g_stage_play = g_play_exec;
    printf("stage list: stage %d of %d, %s, scenario %d%s%s: the game is asked to change stage at its next StageSceneNrvPlay (frame %lld)\n", k + 2, g_list_n,
           g_list[k + 1].stage, g_list[k + 1].scenario, g_list[k + 1].start[0] ? ", start " : "", g_list[k + 1].start, frame);
    fflush(stdout);
}

/* nnhid.c: a stage change is asked for and waits for the scene's StageSceneNrvPlay */
int stage_list_waiting(void) { return g_list_n && g_list_state == LIST_ASKED; }

/* boot.c: when the entry being played began (GetTickCount64, less the time presents were held before; 0 = the first
 * entry, or no list): the run's time limit counts from there */
unsigned long long stage_list_started(void) { return g_list_n ? g_list_tick : 0; }

/* The scenario the game's own stage change gives a stage when none is asked for, as a door without a ScenarioNo does:
 * ChangeStageInfo's constructor then takes GameDataHolder::findUseScenarioNo (1 for a kingdom's challenge rooms, say);
 * -1 when the stage has none (it plays in the kingdom's scenario). Made with the calling thread's registers, put back. */
static int use_scenario(Ctx *c, uint64_t holder, const char *stage) {
    static __declspec(align(16)) unsigned char csi[0x1000];      /* a ChangeStageInfo: 0x278 bytes */
    if (!GAME_FN_ChangeStageInfo_ctor || !holder) return -1;
    Ctx saved = *c;
    memset(csi, 0, sizeof csi);
    c->x[0] = (uint64_t)(uintptr_t)csi;
    c->x[1] = holder;
    c->x[2] = 0;
    c->x[3] = (uint64_t)(uintptr_t)stage;
    c->x[4] = 0;
    c->x[5] = 0xffffffffu;                                /* no scenario asked for */
    c->x[6] = 0;
    call_guest(c, GAME_FN_ChangeStageInfo_ctor);
    *c = saved;
    return *(int32_t *)(csi + 0x1cc);                     /* its scenario: +0x1cc in both versions */
}

/* The GameDataHolder a stage scene is made with (SceneInitInfo +8), when RTTI says it is one; else 0. */
static uint64_t holder_of(uint64_t info) {
    char cls[64] = "?";
    uint64_t h = rd64(info + 8);
    return class_of(h, cls, sizeof cls) && !strcmp(cls, "GameDataHolder") ? h : 0;
}

/* hostrt.c call_indirect, on the thread that runs the stage scene: StageSceneNrvPlay's execute while a change is asked
 * for. The game's own calls are made with this thread's registers, which are put back before its call goes on. */
void stage_play_hit(Ctx *c) {
    g_stage_play = 0;
    if (g_list_state != LIST_ASKED) return;
    int k = g_list_cur + 1;
    static __declspec(align(16)) unsigned char info[0x1000];     /* ChangeStageInfo: 0x278 bytes */
    Ctx saved = *c;
    memset(info, 0, sizeof info);
    c->x[0] = (uint64_t)(uintptr_t)info;
    c->x[1] = g_holder;
    c->x[2] = g_list[k].start[0] ? (uint64_t)(uintptr_t)g_list[k].start : 0;
    c->x[3] = (uint64_t)(uintptr_t)g_list[k].stage;
    c->x[4] = 0;                                          /* not a return to the stage before */
    c->x[5] = (uint32_t)g_list[k].scenario;
    c->x[6] = 0;                                          /* no sub-scenario */
    call_guest(c, GAME_FN_ChangeStageInfo_ctor);
    int scen = *(int32_t *)(info + 0x1cc);                /* its scenario (+0x1cc): the one asked for, else the stage's own, else -1 */
    c->x[0] = g_holder;
    c->x[1] = (uint64_t)(uintptr_t)info;
    call_guest(c, GAME_FN_tryChangeNextStage);
    int ok = (int)(c->x[0] & 1);
    *c = saved;
    InterlockedExchange(&g_list_state, ok ? LIST_CHANGING : LIST_PLAY);
    char from[96];
    if (scen >= 0) snprintf(from, sizeof from, "scenario %d (%s)", scen, g_list[k].scenario >= 0 ? "asked for" : "the stage's own, as through its door");
    else snprintf(from, sizeof from, "the kingdom's scenario");
    printf("stage list: frame %lld: GameDataFunction::tryChangeNextStage to %s, %s%s%s: %s\n", nvn_front_frame_next(), g_list[k].stage, from,
           g_list[k].start[0] ? ", start " : "", g_list[k].start, ok ? "changing" : "REFUSED (no change; the run waits for its time limit)");
    fflush(stdout);
}

/* StageScene::init of a stage of the list: the first, the one a change asked for, or one the game starts itself (a
 * restart after a miss). Called from stage_init_hit with the stage and scenario the scene is made for. */
static void stage_list_init_seen(uint64_t info, const char *stage, int scen) {
    long long frame = nvn_front_frame_next();
    if (!g_holder && !(g_holder = holder_of(info))) {
        printf("stage list: the scene's init info does not hold the GameDataHolder (RTTI): no stage after the first\n");
        g_list_n = 1;
    }
    if (g_stage_inits == 1) {
        printf("stage list: frame %lld: stage 1 of %d begins: the game starts %s, scenario %d%s%s\n", frame, g_list_n, stage, scen,
               strcmp(stage, g_list[0].stage) ? "; NOT the list's first stage, " : "", strcmp(stage, g_list[0].stage) ? g_list[0].stage : "");
        return;
    }
    if (g_list_state != LIST_CHANGING) {
        printf("stage list: frame %lld: the game starts %s, scenario %d itself (no change of the list: a restart after a miss?)\n", frame, stage, scen);
        return;
    }
    int k = InterlockedIncrement(&g_list_cur);
    printf("stage list: frame %lld: stage %d of %d begins: the game starts %s, scenario %d%s%s\n", frame, k + 1, g_list_n, stage, scen,
           strcmp(stage, g_list[k].stage) ? "; NOT the stage asked for, " : "", strcmp(stage, g_list[k].stage) ? g_list[k].stage : "");
    InterlockedExchange(&g_list_state, LIST_BEGUN);
}

/* nnhid.c, each poll: an entry of the list began since the last call: the game-state tracker starts afresh (the old
 * scene's objects are gone; tracking resumes if the input sequence had stopped it) and the entry's input sequence is
 * returned; else NULL. */
static long long g_released;
const char *stage_list_begun(void) {
    if (g_list_state != LIST_BEGUN) return NULL;
    InterlockedExchange(&g_list_state, LIST_PLAY);
    for (int i = 0; i < g_nrv_count; i++) g_nrv[i].last_frame = -10;
    while (InterlockedCompareExchange(&g_nerve_lock, 1, 0)) YieldProcessor();
    memset(g_keep, 0, sizeof g_keep);
    g_keep_used = 0;
    g_player_actor = 0;
    g_player_nrv = -1;
    g_nerve_lock = 0;
    if (g_nerve_mode == 0 && g_released >= 0 && g_nrv_count > 0) {
        g_nerve_mode = 1;
        printf("nerve: tracking resumes at frame %lld (a new stage of SMO_STAGE_LIST)\n", nvn_front_frame_next());
    }
    char key[32];
    snprintf(key, sizeof key, "SMO_SEQ_%d", (int)g_list_cur + 1);
    const char *s = getenv(key), *from = key;
    if (!s) { s = getenv("SMO_STAGE_SEQ"); from = "SMO_STAGE_SEQ"; }
    if (!s) { s = getenv("SMO_SEQ"); from = "SMO_SEQ"; }
    printf("stage list: the input sequence starts again (%s)\n", s ? from : "none");
    return s ? s : "";
}

void stage_init_hit(Ctx *c) {
    int first = g_stage_inits++ == 0;
    uint64_t info = c->x[1];
    __try {
        uint64_t was = rd64(info + 0x18);
        int scen = (int)rd32(info + 0x20);
        char name[96] = "?";
        for (int i = 0; was && i < (int)sizeof name - 1; i++) { name[i] = (char)rd8(was + i); name[i + 1] = 0; if (!name[i]) break; }
        if (!first && !g_list_n)                          /* SMO_START_STAGE alone: later stage scenes are only logged */
            printf("start stage: frame %lld: the game starts %s, scenario %d itself (a later stage scene: a restart after a miss?)\n", nvn_front_frame_next(), name, scen);
        if (first && g_start_stage[0]) {                  /* the scenario: the one asked for, else the stage's own (as through its door), else the game's */
            int own = g_start_scenario >= 0 ? -1 : use_scenario(c, holder_of(info), g_start_stage);
            int to = g_start_scenario >= 0 ? g_start_scenario : own >= 0 ? own : scen;
            wr64(info + 0x18, (uint64_t)(uintptr_t)g_start_stage);
            if (to != scen) wr32(info + 0x20, (uint32_t)to);
            printf("start stage: frame %lld: the game starts %s, scenario %d; SMO_START_STAGE makes it %s, scenario %d (%s)\n", nvn_front_frame_next(),
                   name, scen, g_start_stage, to, g_start_scenario >= 0 ? "asked for" : own >= 0 ? "the stage's own, as through its door" : "the kingdom's");
            snprintf(name, sizeof name, "%s", g_start_stage);
            scen = to;
        }
        if (g_list_n) stage_list_init_seen(info, name, scen);
    } __except (1) {
        printf("start stage: the scene's init info at %llx could not be read: the game's own stage starts\n", (unsigned long long)info);
    }
    fflush(stdout);
}

/* SMO_SEQ "@nerve:<name>": did that nerve run in this frame or the one before? Unknown names are reported once. */
int nerve_active(const char *name) {
    if (g_nerve_mode < 0) nerve_init();
    for (int i = 0; i < g_nrv_count; i++)
        if (!strcmp(g_nrv[i].name, name)) return g_nrv[i].last_frame >= nvn_front_frame_next() - 1;
    static char said[96];
    if (strcmp(said, name)) { snprintf(said, sizeof said, "%s", name); printf("nerve: \"%s\" is not the name of a nerve state (see SMO_NERVELOG=*)\n", name); }
    return 0;
}

/* Camera 0 of an actor's scene, as al::getCameraPos / al::getCameraAt read it: LiveActor::getCameraDirector = scene info
 * (+0xf8) -> director (+8); director +0x10 -> scene camera info, +8 -> views, view 0, +8 -> camera: position +0x38, look-at +0x44 */
static int camera_of(uint64_t actor, float pos[3], float at[3]) {
    __try {
        uint64_t si = rd64(actor + 0xf8), dir = si ? rd64(si + 8) : 0, info = dir ? rd64(dir + 0x10) : 0;
        uint64_t views = info ? rd64(info + 8) : 0, v0 = views ? rd64(views) : 0, cam = v0 ? rd64(v0 + 8) : 0;
        if (!cam) return 0;
        memcpy(pos, (void *)(uintptr_t)(cam + 0x38), 12);
        memcpy(at, (void *)(uintptr_t)(cam + 0x44), 12);
        return 1;
    } __except (1) { return 0; }
}

/* SMO_SEQ "@goto:<x>,<z>[,<r>]" (nnhid.c): the left stick that moves the player toward the world point (x, z) on the
 * ground plane, as the game turns the stick into a direction: up = the camera's forward (look-at minus position, y
 * dropped), right = forward x up (y up). 1 = the player is within r of the point; 0 = *sx, *sy hold the stick (unit
 * length); -1 = no player or camera yet. *dist gets the distance left. */
int nerve_steer(float tx, float tz, float r, float *sx, float *sy, float *dist) {
    float p[3], cp[3], ca[3];
    if (!g_player_actor) return -1;
    memcpy(p, g_player_pos, sizeof p);
    float dx = tx - p[0], dz = tz - p[2], d = sqrtf(dx * dx + dz * dz);
    if (dist) *dist = d;
    if (d <= r) return 1;
    if (!camera_of(g_player_actor, cp, ca)) return -1;
    float fx = ca[0] - cp[0], fz = ca[2] - cp[2], fl = sqrtf(fx * fx + fz * fz);
    if (fl < 1e-3f) return -1;
    fx /= fl; fz /= fl;
    float ux = dx / d, uz = dz / d;
    float y = ux * fx + uz * fz, x = ux * -fz + uz * fx, m = sqrtf(x * x + y * y);
    if (m < 1e-6f) return -1;
    *sx = x / m; *sy = y / m;
    return 0;
}
int nerve_player(float pos[3]) { if (!g_player_actor) return 0; memcpy(pos, g_player_pos, 12); return 1; }
const char *nerve_player_nerve(void) { return g_player_nrv >= 0 ? g_nrv[g_player_nrv].name : ""; }
uint64_t nerve_player_actor(void) { return g_player_actor; }        /* changes when a new stage makes a new player */

/* SMO_SEQ "@chase:<prefix>" (nnhid.c): the position of an actor whose current nerve name starts with prefix (the keeper
 * found last is checked first; the keeper table is searched again when its nerve no longer matches). 1 = found. */
int nerve_actor_pos(const char *prefix, float pos[3]) {
    static uint64_t cached;
    static char cached_prefix[96];
    size_t n = strlen(prefix);
    int found = 0;
    while (InterlockedCompareExchange(&g_nerve_lock, 1, 0)) YieldProcessor();
    if (strcmp(cached_prefix, prefix)) { snprintf(cached_prefix, sizeof cached_prefix, "%s", prefix); cached = 0; }
    long long recent = nvn_front_frame_next() - 2;
    if (cached) {                                         /* the keeper found last: its slot by hash */
        unsigned h = hash_addr(cached, KEEP_HASH);
        while (g_keep[h].keeper && g_keep[h].keeper != cached) h = (h + 1) % KEEP_HASH;
        if (g_keep[h].keeper == cached && !strncmp(g_nrv[g_keep[h].nrv].name, prefix, n) && g_keep[h].last_frame >= recent)
            found = actor_trans(cached, pos);
    }
    if (!found) {                                         /* else the nearest to the player of the keepers whose nerve matches and ran recently */
        float best = 3.4e38f, p[3];
        for (unsigned h = 0; h < KEEP_HASH; h++) {
            if (!g_keep[h].keeper || strncmp(g_nrv[g_keep[h].nrv].name, prefix, n) || g_keep[h].last_frame < recent) continue;
            if (!actor_trans(g_keep[h].keeper, p)) continue;
            float dx = p[0] - g_player_pos[0], dy = p[1] - g_player_pos[1], dz = p[2] - g_player_pos[2], d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < best) { best = d2; memcpy(pos, p, sizeof p); cached = g_keep[h].keeper; found = 1; }
        }
    }
    g_nerve_lock = 0;
    return found;
}

/* SMO_SEQ: no step left in the input sequence needs the tracker. Unless SMO_NERVELOG or SMO_ACTORPOS want it, it stops,
 * so that the rest of a run (a timing run, say) is not slowed by it. */
static long long g_released = -1;
void nerve_release(void) {
    if (g_nerve_mode <= 0 || g_filter[0] || g_pos_every > 0) return;
    g_nerve_mode = 0;
    g_released = nvn_front_frame_next();
    printf("nerve: tracking stops at frame %lld: no step of the input sequence needs it any more\n", g_released);
}

void nerve_report(FILE *out) {
    if (g_nerve_mode <= 0 && g_released < 0) return;
    if (g_released >= 0) fprintf(out, "nerve: tracking stopped at frame %lld (input sequence)\n", g_released);
    int seen = 0;
    for (int i = 0; i < g_nrv_count; i++) seen += g_nrv[i].last_frame >= 0;
    fprintf(out, "nerve: %d of %d nerve states ran; %lld state changes (%lld printed); keeper table cleared %d times\n",
            seen, g_nrv_count, g_transitions, g_logged, g_keep_clears);
    if (g_player_nrv >= 0)
        fprintf(out, "nerve: player last seen at %.1f %.1f %.1f in %s\n", g_player_pos[0], g_player_pos[1], g_player_pos[2], g_nrv[g_player_nrv].name);
}
