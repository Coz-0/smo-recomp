/* smo_nerve.c - SMO port, test aid: which state (nerve) each of the game's state machines is in.
 *
 * A state of an al::NerveKeeper is a const al::Nerve; al::NerveKeeper::update calls its execute(NerveKeeper *) once per
 * frame through the vtable, so through call_indirect (hostrt.c). The function list (tools/extract/functions.py -> build\functions\
 * main_functions.tsv) names such execute functions: by symbol or the decompilation's name (1.0.0: 5,998), and by the
 * class RTTI gives (1.0.0: 6,410, the same names wherever both exist; 1.4.1: the only source). The others are named
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
 *
 * A nerve's name is its mangled nested name without the anonymous namespace, joined with "::"
 * (_ZNK12_GLOBAL__N_135OpeningStageStartDemoNrvWaitStandUp7executeEPN2al11NerveKeeperE -> OpeningStageStartDemoNrvWaitStandUp).
 * Reads game memory only; never changes it. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
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

/* Nerves come from the function list by their names: the symbol, else the decompilation's name, else the name RTTI
 * gives (column 9, tools/extract/functions.py: the only one for 1.4.1, whose main has no symbols). Many classes are
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
        char *col[9];
        int nc = 0;
        for (char *s = line; nc < 9; ) {
            col[nc++] = s;
            char *t = strchr(s, '\t');
            if (!t) { s[strcspn(s, "\r\n")] = 0; break; }
            *t = 0;
            s = t + 1;
        }
        if (nc < 7 || !strcmp(col[0], "start")) continue;
        uint64_t a = GUEST_BASE + strtoull(col[0], NULL, 16), e = GUEST_BASE + strtoull(col[1], NULL, 16);
        char *mangled = col[5][0] ? col[5] : col[6];
        mangled[strcspn(mangled, "|")] = 0;
        int rtti_other = 0;                               /* an RTTI name that is not a state (a class's own executeOnEnd) */
        if (nc > 8 && col[8][0]) have_rtti = 1;
        if (nerve_name(mangled, nm, sizeof nm)) { nerve_add(a, nm); named++; continue; }
        if (nc > 8 && col[8][0]) {
            if (nerve_name(col[8], nm, sizeof nm)) {
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
     * they name every nerve class, so what is left unnamed is not a nerve (1.4.1: 80 such guesses). */
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

static void nerve_init(void) {                           /* first indirect call (any thread); the others wait for it */
    static volatile LONG once;
    if (InterlockedCompareExchange(&once, 1, 0)) { while (*(volatile int *)&g_nerve_mode < 0) YieldProcessor(); return; }
    const char *log = getenv("SMO_NERVELOG"), *pos = getenv("SMO_ACTORPOS"), *seq = getenv("SMO_SEQ");
    int want = (log && *log) || (pos && atoi(pos) > 0) || (seq && (strstr(seq, "@nerve:") || strstr(seq, "@until-nerve:") || strstr(seq, "@goto:") ||
                                                                    strstr(seq, "@near:") || strstr(seq, "@at:") || strstr(seq, "@chase:")));
    if (log) { snprintf(g_filter, sizeof g_filter, "%s", log); g_filter_all = !strcmp(log, "*"); }
    g_pos_every = pos ? atoi(pos) : 0;
    if (pos && strchr(pos, ':')) snprintf(g_pos_prefix, sizeof g_pos_prefix, "%s", strchr(pos, ':') + 1);
    if (want) nerve_load();
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
    if (!strncmp(g_nrv[i].name, "PlayerActorHakoniwaNrv", 22) && actor_trans(keeper, g_player_pos)) { g_player_nrv = i; g_player_actor = rd64(keeper); }
    if (g_nrv[i].pos_ok && (g_keep[h].pos_frame < 0 || frame - g_keep[h].pos_frame >= g_pos_every)) {
        float p[3];
        if (actor_trans(keeper, p)) {
            g_keep[h].pos_frame = frame;
            printf("actor: frame %lld keeper %llx pos %.1f %.1f %.1f nerve %s\n", frame, (unsigned long long)keeper, p[0], p[1], p[2], g_nrv[i].name);
        }
    }
    g_nerve_lock = 0;
}

/* SMO port, the amiibo menu (smo_amiibo.c, nnhid.c): two nerve states watched without the tracker. hostrt.c call_indirect
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
