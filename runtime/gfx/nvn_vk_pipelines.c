/* stages, pipelines and the worker threads that make them AHEAD of the draw.
 * Included by nvn_vk_draw.c (not a build unit of its own).
 *
 *   - nvnProgramSetShaders (guest thread) -> nvn_vk_program_set: a copy of the program's machine code goes to the worker
 *     queue at once. A worker translates each stage (mx_translate -> glslang -> module, or the record on disk), then makes
 *     pipelines for the pair: the keys of ..\cache\vk\pipes.bin were all made at start; for a pair with no recorded key the
 *     keys are PREDICTED = the pair x the pass signatures most used so far (at most 3), triangles.
 *   - a draw looks up; when its stage or pipeline is being made by a worker it WAITS for it, when nobody makes it it makes
 *     it itself. Either way the render thread stands still at that draw: the present is late, no draw is dropped or
 *     reordered. Every such hold is counted (g_hold_*; "holds so far" in the nvn_vk_perf line).
 *   - with VK_EXT_extended_dynamic_state3 blend enable / equation / write mask, polygon mode, alpha to coverage, depth
 *     clamp and the sample mask are dynamic state, so the key is (stages, topology class, pass formats, samples, logic op,
 *     sample shading): few keys per pair, which is what makes the prediction possible.
 * One lock (g_sh_lock) guards the stage table, the pipeline table, the signature / pair tables and the job queue. */
#include <psapi.h>
typedef struct { uint8_t unit, dim, shadow; uint16_t binding; } VSmp;
typedef struct VStage {
    uint64_t h, hb;                                /* h: this variant (code, constants, canonical key, forced outputs); hb: code and constants only */
    int stage, code_len, ok; volatile LONG state, cp_state;      /* state: 1 being made, 2 made (ok says how it went); cp_state: compute pipeline 0 none, 1 being made, 2 made */
    int gprim;                                     /* a geometry stage: the primitive class of the draws it serves (MxIn.gs_in_prim; part of hb) */
    uint32_t images_w; uint16_t img_fmt[8]; uint8_t img_type[8];      /* a compute stage's images (MxOut): written ones, declared format and type */
    uint32_t key[4], fo, in_attr, out_attr, id, cbufs, ssbos, ssbo_w, images, dep, tex_units; int uses_alpha, uses_warp, nsmp, cp_recorded, from_rec, fragonly;      /* uses_warp: MxOut; fragonly: handed over by a program without a vertex stage */
    VkPipeline cp; VkPipelineLayout cl; VkDescriptorSetLayout cdl[2];      /* compute stage: its pipeline */
    uint32_t local[3];
    VSmp smp[64];
    VkShaderModule mod; struct VStage *next, *next_h;
    VkPipeline lib, lib2; volatile LONG lib_state, lib2_state;      /* SMO_VK_GPL=2: the stage's pipeline library; lib2: a vertex stage with the empty fragment shader state */
    uint64_t spv; int mod_users, mod_rel;          /* spv: name of the stage's SPIR-V file; the module is used by mod_users threads now and may be released (mod_rel) once its library or compute pipeline is made (stage_mod_get) */
} VStage;
#define MX_VK_VERSION 11u                        /* part of every hash: change it when mx_translate's Vulkan text or these records change */
typedef struct { uint32_t magic; int32_t stage, code_len; uint32_t key[4], fo; uint64_t spv, hb; uint32_t cbufs, ssbos, ssbo_w, images, dep, tex_units, in_attr, out_attr; int32_t uses_alpha, nsmp, uses_warp; VSmp smp[64]; } StageRec;
typedef struct { uint32_t magic, key[4], fo, dep, tex; uint64_t vh; } VarRec;      /* ..\cache\vk\stage\<stage>_<hb>.var: the variants of one code */
#define STAGEREC_MAGIC (0x52545356u + MX_VK_VERSION)
typedef struct { int stage, code_len, serial; const uint8_t *code, *cdata; uint32_t cdata_size, local[3], smem, lmem; int gprim; /* geometry stage, as VStage */ } SSrc;
static CRITICAL_SECTION g_sh_lock;
static CONDITION_VARIABLE g_sh_cv, g_jq_cv;        /* g_sh_cv: something was finished; g_jq_cv: a job was queued */
/* ---- the freeze watchdog's wait registry (nvn_vk_watchdog.c writes the report). Every wait of ours that has no
 * time-out, and the long ones, are entered here with the function and line that waits; the report names them. */
typedef struct { volatile LONG tid; const char *fn; int line; LONG64 since; } WdWait;
static WdWait g_wd_wait[64];
static volatile LONG64 g_wd_beat;                  /* QueryPerformanceCounter of the last present (normal or progress bar) */
static volatile LONG g_wd_where;                   /* the render thread's place in a present (1 barrier, 2 acquire, 4 present) */
static volatile LONG64 g_wd_try;                /* present attempts (one that failed or was skipped counts here, not in g_wd_beat) */
static int wd_enter(const char *fn, int line) {
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    LONG me = (LONG)GetCurrentThreadId();
    for (int i = 0; i < 64; i++) if (!g_wd_wait[i].tid && InterlockedCompareExchange(&g_wd_wait[i].tid, me, 0) == 0) { g_wd_wait[i].fn = fn; g_wd_wait[i].line = line; g_wd_wait[i].since = t.QuadPart; return i; }
    return -1;
}
static void wd_leave(int i) { if (i >= 0) g_wd_wait[i].tid = 0; }
#define WD_SLEEP(cv, lock, ms) do { int wd_i_ = wd_enter(__func__, __LINE__); SleepConditionVariableCS(cv, lock, ms); wd_leave(wd_i_); } while (0)
static VkPipelineCache g_pcache;
static LONG64 g_pw_stages, g_pw_pipes, g_pw_failed, g_pipe_miss;
static VStage *g_vstage[4096], *g_vh[4096];        /* by hb, by h */
static uint32_t g_vstage_ids;
static int (*g_glsl_to_spirv)(int, const char *, uint32_t **, size_t *, char *, size_t);
static void (*g_spirv_free)(uint32_t *);
enum { H_STAGE_DRAW, H_STAGE_WAIT, H_PIPE_DRAW, H_PIPE_WAIT, H_N };
static LONG64 g_hold_n[H_N], g_hold_tk[H_N], g_hold_max;
static LONG64 g_w_progs, g_w_stages, g_w_pipes, g_pred_made, g_pred_used, g_set_held, g_set_held_tk;
static void hold_note(int what, LONG64 ticks) { g_hold_n[what]++; g_hold_tk[what] += ticks; if (ticks > g_hold_max) g_hold_max = ticks; }
static int holdlog_on(void) { static int on = -1; if (on < 0) on = getenv("SMO_VK_HOLDLOG") && atoi(getenv("SMO_VK_HOLDLOG")); return on; }      /* a "nvn_vk_hold:" line for every stage / pipeline made at a draw */
/* g_in_scene: the last frame had compute dispatches. The game dispatches compute in the frames that draw a scene, not
 * in every frame of its boot and loading screens ("nvn_vk_frames:" at exit counts both). It is per frame, and a scene can
 * have a frame without a dispatch: "near a scene", for the purpose of NOT holding a caller, is a scene frame within the
 * last 600 frames (scene_recent). */
static volatile LONG g_in_scene;
static volatile LONG64 g_scene_last = -100000;
static int scene_recent(void) { return g_in_scene || nvn_front_frame_next() - g_scene_last < 600; }
static LONG64 g_var_known;
static LONG64 g_jq_queued, g_jq_done, g_bq_queued, g_bq_done;
/* SMO_VK_HOLDLOG=1 only: when a stage's code was first handed to the workers (nvnProgramSetShaders), so that a hold
 * can say whether anyone knew the code before its draw. */
static struct { uint64_t hb; int32_t frame; } g_ho[65536];
static void ho_note(uint64_t hb) {
    unsigned i = (unsigned)(hb * 0x9E3779B97F4A7C15ull >> 48);
    for (int n = 0; n < 16; n++, i = (i + 1) & 65535) { if (g_ho[i].hb == hb) return; if (!g_ho[i].hb) { g_ho[i].hb = hb; g_ho[i].frame = (int32_t)nvn_front_frame_next(); return; } }
}
static const char *ho_text(uint64_t hb) {
    static char b[96];
    unsigned i = (unsigned)(hb * 0x9E3779B97F4A7C15ull >> 48);
    for (int n = 0; n < 16; n++, i = (i + 1) & 65535) if (g_ho[i].hb == hb) { snprintf(b, sizeof b, "code first handed over at frame %d by nvnProgramSetShaders", g_ho[i].frame); return b; }
    return "code never handed over before this draw";
}
static uint64_t fnv(uint64_t h, const void *p, size_t n) { const uint8_t *b = p; for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001B3ULL; return h; }      /* byte-wise: names and records of the cache files (spv, stage, pipes.bin): never change it */
/* Word-wise, for the tables in memory that are hashed at every draw (texture-set lists, pipeline keys, look-ahead tags):
 * one multiply per 8 bytes instead of one per byte (fnv's chain of dependent multiplies), a byte loop for the tail, a final
 * mix. Never for a value that names, or is stored in, a cache file. */
static uint64_t hmix(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (; n >= 8; n -= 8, b += 8) { uint64_t w; memcpy(&w, b, 8); h = (h ^ w) * 0x9E3779B97F4A7C15ull; h ^= h >> 29; }
    for (; n; n--, b++) h = (h ^ *b) * 0x100000001B3ULL;
    h ^= h >> 32; h *= 0xD6E8FEB86659FD93ull; h ^= h >> 32;      /* every bit into the low ones (the tables index by them) */
    return h;
}
/* the cache folder. SMO_VK_CACHE=<folder> (tests: a first run on a cache of one's own, without touching the
 * shared ..\cache\vk); first called on the starting thread, before the workers exist. */
static const char *vkc_dir(void) {
    static char d[200];
    if (!d[0]) { const char *e = getenv("SMO_VK_CACHE"); snprintf(d, sizeof d, "%s", e && e[0] ? e : "../cache/vk"); }
    return d;
}
static const char *vkc_file(const char *name) { static char p[4][260]; static LONG i; char *b = p[InterlockedIncrement(&i) & 3]; snprintf(b, 260, "%s/%s", vkc_dir(), name); return b; }
static void compute_pipe_make(VStage *v);
static uint64_t g_spv_unused;
static PFN_vkCmdSetColorBlendEnableEXT vkCmdSetColorBlendEnableEXT;
static PFN_vkCmdSetColorBlendEquationEXT vkCmdSetColorBlendEquationEXT;
static PFN_vkCmdSetColorWriteMaskEXT vkCmdSetColorWriteMaskEXT;
static PFN_vkCmdSetPolygonModeEXT vkCmdSetPolygonModeEXT;
static PFN_vkCmdSetAlphaToCoverageEnableEXT vkCmdSetAlphaToCoverageEnableEXT;
static PFN_vkCmdSetDepthClampEnableEXT vkCmdSetDepthClampEnableEXT;
static PFN_vkCmdSetSampleMaskEXT vkCmdSetSampleMaskEXT;

/* ---------------------------------------------------------------- stages: machine code -> Vulkan GLSL -> SPIR-V -> module */
static int ssrc_of(SSrc *o, const Prog *p, int s) {
    const Stage *st = &p->st[s];
    memset(o, 0, sizeof *o);
    if (!st->code || st->code_len <= 0) return 0;
    o->stage = s; o->code = st->code; o->code_len = st->code_len; o->serial = p->serial;
    if (st->ctl[3] && st->ctl[4]) { o->cdata = st->code - (s == 5 ? 0 : 0x30) + st->ctl[4]; o->cdata_size = st->ctl[3]; }      /* a compute stage has no program header */
    memcpy(o->local, st->local, sizeof o->local); o->smem = st->smem_bytes; o->lmem = st->lmem_bytes;
    return 1;
}
static uint64_t ssrc_hb(const SSrc *o) {
    uint32_t ver = MX_VK_VERSION;
    uint64_t h = fnv(0xcbf29ce484222325ULL ^ (uint64_t)o->stage, o->code, (size_t)o->code_len);
    if (o->stage == 5) { h = fnv(h, o->local, sizeof o->local); h = fnv(h, &o->smem, 4); h = fnv(h, &o->lmem, 4); }
    if (o->cdata) h = fnv(h ^ o->cdata_size, o->cdata, o->cdata_size);
    if (o->stage == 2) h = fnv(h, &o->gprim, sizeof o->gprim);      /* a geometry stage is translated for the draw's primitive class */
    if (o->stage == 0 && o->code_len > 0x35 && (o->code[0x35] >> 3 & 1)) h = fnv(h, "ps1", 3);      /* a vertex stage whose program header says it writes a point size (output map B bit 3): translated to draw with it, another translation than the push constant's; every other stage keeps its hash */
    if (o->stage == 1 && RS_ON) h = fnv(h, "rs1", 3);      /* the fragment stages of a run with an internal resolution scale are another translation (MxIn.screen_scale); at 1.0 the hash is the one it always was */
    return fnv(h, &ver, 4);
}
static VkShaderModule module_from_spv(uint64_t h) {              /* any thread; NULL when the file is not there */
    char path[128];
    VkShaderModule mod = NULL;
    snprintf(path, sizeof path, "%s/spv/%016llx.spv", vkc_dir(), (unsigned long long)h);
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
    uint32_t *words = n > 20 && n % 4 == 0 ? malloc((size_t)n) : NULL;
    if (words && fread(words, 1, (size_t)n, fp) == (size_t)n && words[0] == 0x07230203) {
        VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = (size_t)n; ci.pCode = words;
        if (vkCreateShaderModule(V.device, &ci, NULL, &mod) != VK_SUCCESS) mod = NULL;
    }
    free(words); fclose(fp);
    return mod;
}
static VkShaderModule module_from_glsl(int stage, const char *glsl, const char *what, uint64_t *spv_hash, int *on_disk) {      /* any thread; *on_disk (may be NULL): the SPIR-V file is there to be read again */
    {   /* debug aid SMO_VK_GLSLDUMP=<folder>: every freshly translated stage as <what>.glsl (stages found in the
         * stage cache are not translated again: clear build/cache/vk/stage and spv to dump everything) */
        const char *dir = getenv("SMO_VK_GLSLDUMP");
        if (dir) {
            static LONG n;
            char dp[300]; snprintf(dp, sizeof dp, "%s/%s.glsl", dir, what); for (char *q = dp + strlen(dir) + 1; *q; q++) if (*q == ' ' || *q == '#') *q = '_';
            FILE *f = fopen(dp, "wb");
            if (f) { fputs(glsl, f); fclose(f); }
            if (InterlockedIncrement(&n) <= 2) printf("nvn_vk: GLSL dump %s: %s\n", dp, f ? "written" : "NOT written");
        }
    }
    char path[128], log[2000];
    uint32_t *words = NULL;
    size_t count = 0;
    VkShaderModule mod = NULL;
    uint64_t h = fnv(0xcbf29ce484222325ULL ^ (uint64_t)stage ^ 0x5601, glsl, strlen(glsl));
    *spv_hash = h;
    if (on_disk) *on_disk = 0;
    if ((mod = module_from_spv(h)) != NULL) { InterlockedIncrement64(&R.modules); if (on_disk) *on_disk = 1; return mod; }
    if (!g_glsl_to_spirv(stage, glsl, &words, &count, log, sizeof log)) { printf("nvn_vk: %s: glslang rejected the translated text: %s\n", what, log); fflush(stdout); return NULL; }
    snprintf(path, sizeof path, "%s/spv/%016llx.spv", vkc_dir(), (unsigned long long)h);
    FILE *fp = fopen(path, "wb");
    if (fp) { int ok = fwrite(words, 4, count, fp) == count; if (fclose(fp) == 0 && ok && on_disk) *on_disk = 1; }
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = count * 4; ci.pCode = words;
    VKC(vkCreateShaderModule(V.device, &ci, NULL, &mod));
    g_spirv_free(words);
    InterlockedIncrement64(&R.modules);
    return mod;
}
static int stage_rec_read(int s, uint64_t vh, StageRec *r) {
    char path[128];
    snprintf(path, sizeof path, "%s/stage/%d_%016llx.st", vkc_dir(), s, (unsigned long long)vh);
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    size_t got = fread(r, 1, sizeof *r, fp);
    fclose(fp);
    return got == sizeof *r && r->magic == STAGEREC_MAGIC && r->stage == s && r->nsmp >= 0 && r->nsmp <= 64;
}
/* A stage's module once its library (or compute pipeline) is made: the driver keeps a copy of each module's SPIR-V (58 KB
 * on average, 15,800 modules after the game hands its programs over at start: about 0.9 GB per game) although nothing
 * needs the module again unless a pipeline of another kind is made from the stage (a pair library, a whole pipeline, a
 * vertex stage drawn alone). So the module is destroyed when no thread uses it, and made again from the SPIR-V file of the
 * stage cache when it is needed: the same words, so the same pipelines. SMO_VK_MODREL=0 keeps every module (as before). */
static LONG64 g_mod_released, g_mod_again, g_mod_destroy_tk;
static int modrel_on(void) { static int on = -1; if (on < 0) on = getenv("SMO_VK_MODREL") ? atoi(getenv("SMO_VK_MODREL")) : 1; return on; }      /* 2 (measurement): released as soon as the stage is made */
static VkShaderModule module_from_spv(uint64_t h);
static VkShaderModule stage_mod_get(VStage *v) {   /* any thread, no lock held; stage_mod_put when the module is no longer used */
    EnterCriticalSection(&g_sh_lock);
    v->mod_users++;
    if (!v->mod && v->spv) {
        v->mod = module_from_spv(v->spv);
        if (!v->mod) { LeaveCriticalSection(&g_sh_lock); vkd_fail("the SPIR-V file %016llx of a stage whose module was released cannot be read again (was the stage cache folder removed while the game ran?)", (unsigned long long)v->spv); }
        g_mod_again++;
    }
    VkShaderModule m = v->mod;
    LeaveCriticalSection(&g_sh_lock);
    return m;
}
static void stage_mod_put(VStage *v) {
    VkShaderModule m = NULL;
    EnterCriticalSection(&g_sh_lock);
    if (--v->mod_users == 0 && v->mod_rel && v->spv && modrel_on()) { m = v->mod; v->mod = NULL; if (m) g_mod_released++; }
    LeaveCriticalSection(&g_sh_lock);
    if (m) { LONG64 t0 = tk(); vkDestroyShaderModule(V.device, m, NULL); InterlockedAdd64(&g_mod_destroy_tk, tk() - t0); }
}
static void stage_mod_releasable(VStage *v) {      /* its library or compute pipeline is made: the module goes when its last user is done */
    if (!v->spv) return;                /* spv 0: no SPIR-V file to make it again from, the module is kept */
    EnterCriticalSection(&g_sh_lock);
    v->mod_rel = 1;
    LeaveCriticalSection(&g_sh_lock);
}
static void vstage_from_rec(VStage *v, const StageRec *r, uint64_t vh, VkShaderModule mod) {
    v->h = vh; v->hb = r->hb; v->code_len = r->code_len; memcpy(v->key, r->key, 16); v->fo = r->fo; v->spv = r->spv;
    v->cbufs = r->cbufs; v->ssbos = r->ssbos; v->ssbo_w = r->ssbo_w; v->images = r->images; v->dep = r->dep; v->tex_units = r->tex_units; v->in_attr = r->in_attr; v->out_attr = r->out_attr;
    v->uses_alpha = r->uses_alpha; v->uses_warp = r->uses_warp; v->nsmp = r->nsmp; memcpy(v->smp, r->smp, sizeof v->smp);
    v->mod = mod; v->ok = 1;
}
/* A stage with warp operations runs in subgroups of 32 lanes, a Maxwell warp, where the driver lets the stage's subgroup
 * size be set (Vulkan 1.3 subgroupSizeControl; D.warp32_stages): that is the only way on a GPU whose subgroups can be
 * smaller (Intel: 8 to 32). Elsewhere its text still works on any subgroup of 32 or more (mx_translate.c, MXOP_SHFL). A
 * compute stage whose work group width is a multiple of 32 also gets full subgroups, as a warp on the console. */
static void stage_warp32(VkPipelineShaderStageCreateInfo *st, const VStage *v) {
    static const VkPipelineShaderStageRequiredSubgroupSizeCreateInfo sz = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO, NULL, 32};
    if (!v || !v->uses_warp || !(D.warp32_stages & st->stage)) return;
    st->pNext = &sz;
    if (st->stage == VK_SHADER_STAGE_COMPUTE_BIT && D.f13.computeFullSubgroups && v->local[0] % 32 == 0 && mx_split_x(v->local, 0) == 1)      /* a split group's width is another */
        st->flags |= VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT;
}
static int key_fits(int s, const uint32_t *want, const uint32_t *have, uint32_t dep, uint32_t tex) {
    uint32_t da = s == 0 ? dep : 0;
    return !((want[0] ^ have[0]) & da) && !((want[1] ^ have[1]) & da) && !((want[2] ^ have[2]) & tex) && !((want[3] ^ have[3]) & tex);
}
/* g_sh_lock held. The variant of this code that serves `key` (compared under the variant's own masks: a stage that reads no
 * integer attribute and no integer texture has ONE variant whatever the draw's state is). *busy: a variant of the same code
 * still in the making, whose masks are not known yet. */
static VStage *vstage_find(uint64_t hb, int s, int code_len, const uint32_t *key, uint32_t fo, VStage **busy) {
    *busy = NULL;
    for (VStage *v = g_vstage[hb & 4095]; v; v = v->next) {
        if (v->hb != hb || v->stage != s || v->code_len != code_len || v->fo != fo) continue;
        if (v->state < 2) { if (!memcmp(v->key, key, 16)) return v; *busy = v; continue; }
        if (key_fits(s, key, v->key, v->dep, v->tex_units)) return v;
    }
    return NULL;
}
static VStage *vstage_new(uint64_t hb, int s, int code_len, const uint32_t *key, uint32_t fo) {      /* g_sh_lock held */
    VStage *v = calloc(1, sizeof *v);
    v->hb = hb; v->stage = s; v->code_len = code_len; memcpy(v->key, key, 16); v->fo = fo; v->id = ++g_vstage_ids; v->state = 1;
    v->next = g_vstage[hb & 4095]; g_vstage[hb & 4095] = v;
    return v;
}
/* Any thread, no lock: fills v (in the table, state 1) from its record on disk, else with the translator and glslang. */
static void vstage_build(VStage *v, const SSrc *src) {
    int s = v->stage, done = 0;
    v->gprim = src->gprim;
    char path[128];
    StageRec r;
    snprintf(path, sizeof path, "%s/stage/%d_%016llx.var", vkc_dir(), s, (unsigned long long)v->hb);
    FILE *fp = fopen(path, "rb");
    if (fp) {
        VarRec e;
        while (!done && fread(&e, 1, sizeof e, fp) == sizeof e) {
            VkShaderModule mod;
            if (e.magic != STAGEREC_MAGIC || e.fo != v->fo || !key_fits(s, v->key, e.key, e.dep, e.tex)) continue;
            if (stage_rec_read(s, e.vh, &r) && !r.images && (mod = module_from_spv(r.spv)) != NULL) { vstage_from_rec(v, &r, e.vh, mod); v->from_rec = 1; InterlockedIncrement64(&R.modules); done = 1; }      /* not a stage with images (their formats and types are not in the record) */
        }
        fclose(fp);
    }
    if (!done) {
        MxIn in;
        MxOut out;
        char what[64];
        memset(&in, 0, sizeof in); memset(&out, 0, sizeof out);
        in.stage = s; in.code = src->code; in.code_len = src->code_len; in.cdata = src->cdata; in.cdata_size = src->cdata ? src->cdata_size : 0;
        in.int_attrs = v->key[0]; in.sint_attrs = v->key[1]; in.int_tex = v->key[2]; in.sint_tex = v->key[3]; in.force_out = v->fo; in.gs_in_prim = src->gprim;
        in.target = 1; in.binding_base = 40u * (unsigned)s; in.ssbo_base = 40u * (unsigned)s + 20; in.tex_base = 40u * (unsigned)s; in.image_base = 40u * (unsigned)s + 32; in.wmask_binding = 40u * (unsigned)s + 38;
        if (s == 5) {                              /* compute has a layout of its own (design, section 4): the bindings of stage 0 */
            in.binding_base = 0; in.ssbo_base = 20; in.tex_base = 0; in.image_base = 32; in.wmask_binding = 38; in.track_writes = 1;
            memcpy(in.local_size, src->local, sizeof in.local_size); in.smem_bytes = src->smem; in.lmem_bytes = src->lmem;
        }
        snprintf(what, sizeof what, "program #%d %s stage", src->serial, s == 5 ? "compute" : s == 2 ? "geometry" : s ? "fragment" : "vertex");
        if (s == 1 && RS_ON) in.screen_scale = 1;
        {   /* debug aid: SMO_VK_DUMP_SERIAL=<n> writes the machine code of program n's stages (vk_prog_<n>_<stage>.bin in SMO_SHADERDUMP or the run folder) */
            static int want = -2; if (want == -2) want = getenv("SMO_VK_DUMP_SERIAL") ? atoi(getenv("SMO_VK_DUMP_SERIAL")) : -1;
            static long minb = -2; if (minb == -2) minb = getenv("SMO_VK_DUMP_MINBYTES") ? atol(getenv("SMO_VK_DUMP_MINBYTES")) : -1;
            if ((want >= 0 && src->serial == want) || (minb >= 0 && src->code_len >= minb)) { const char *dir = getenv("SMO_SHADERDUMP"); char path[300]; snprintf(path, sizeof path, "%s%svk_prog_%05d_%d.bin", dir ? dir : "", dir ? "/" : "", src->serial, s); FILE *f = fopen(path, "wb"); if (f) { fwrite(src->code, 1, (size_t)src->code_len, f); fclose(f); } }
        }
        if (!mx_translate(&in, &out)) {
            printf("nvn_vk: %s: machine code not translated: %s\n", what, out.why); fflush(stdout);
            {   /* debug aid: SMO_SHADERDUMP=<dir> also writes the machine code of a stage the translator refuses (as the OpenGL path's b*.bin) */
                const char *dir = getenv("SMO_SHADERDUMP");
                if (dir) { char path[300]; snprintf(path, sizeof path, "%s/vk_fail_%05d_%d.bin", dir, src->serial, s); FILE *f = fopen(path, "wb"); if (f) { fwrite(src->code, 1, (size_t)src->code_len, f); fclose(f); } }
            }
        }
        else {
            uint64_t spv_hash = 0;
            if (in.screen_scale) { InterlockedIncrement64(&g_rs_stages); if (strstr(out.glsl, "mx_fcs)")) InterlockedIncrement64(&g_rs_fcs); if (strstr(out.glsl, "mx_tf(")) InterlockedIncrement64(&g_rs_tf); }
            int on_disk;
            v->mod = module_from_glsl(s, out.glsl, what, &spv_hash, &on_disk);
            v->spv = on_disk ? spv_hash : 0;       /* no SPIR-V file: the module is kept (stage_mod_releasable) */
            v->cbufs = out.cbufs; v->ssbos = out.ssbos; v->ssbo_w = out.ssbo_written; v->images = out.images; v->in_attr = out.in_attr; v->out_attr = out.out_attr;
            v->images_w = out.images_written; memcpy(v->img_fmt, out.image_gl_format, sizeof v->img_fmt); memcpy(v->img_type, out.image_type, sizeof v->img_type);
            v->dep = s == 0 ? out.int_dep : 0; v->tex_units = out.tex_units; v->uses_alpha = out.uses_alpha; v->uses_warp = out.uses_warp; v->nsmp = out.nsmp;
            for (int i = 0; i < out.nsmp && i < 64; i++) {
                const char *ty = out.smp[i].type;
                int arr = strstr(ty, "Array") != NULL, dim = strstr(ty, "Cube") ? (arr ? DIM_CUBEA : DIM_CUBE) : strstr(ty, "3D") ? DIM_3D : strstr(ty, "1D") ? (arr ? DIM_1DA : DIM_1D) : (arr ? DIM_2DA : DIM_2D);
                if (strstr(ty, "MS") || strstr(ty, "Rect") || strstr(ty, "Buffer")) { static char msg[96]; snprintf(msg, sizeof msg, "a shader sampler of type %s", ty); nvn_vk_missing(msg); }
                v->smp[i].unit = out.smp[i].unit; v->smp[i].dim = (uint8_t)dim; v->smp[i].shadow = out.smp[i].shadow; v->smp[i].binding = out.smp[i].binding;
            }
            free(out.glsl);
            v->ok = v->mod != NULL;
            if (v->ok) {                           /* the canonical key: only the bits the code looks at */
                VarRec e;
                v->key[0] &= v->dep; v->key[1] &= v->dep; v->key[2] &= v->tex_units; v->key[3] &= v->tex_units;
                v->h = fnv(fnv(v->hb, v->key, 16), &v->fo, 4);
                if (v->images) goto no_record;     /* a stage with images has no record (its images' formats and types are not in it; see above) */
                memset(&r, 0, sizeof r);
                r.magic = STAGEREC_MAGIC; r.stage = s; r.code_len = v->code_len; memcpy(r.key, v->key, 16); r.fo = v->fo; r.spv = spv_hash; r.hb = v->hb;
                r.cbufs = v->cbufs; r.ssbos = v->ssbos; r.ssbo_w = v->ssbo_w; r.images = v->images; r.dep = v->dep; r.tex_units = v->tex_units; r.in_attr = v->in_attr; r.out_attr = v->out_attr;
                r.uses_alpha = v->uses_alpha; r.uses_warp = v->uses_warp; r.nsmp = v->nsmp; memcpy(r.smp, v->smp, sizeof r.smp);
                snprintf(path, sizeof path, "%s/stage/%d_%016llx.st", vkc_dir(), s, (unsigned long long)v->h);
                if ((fp = fopen(path, "wb")) != NULL) { fwrite(&r, 1, sizeof r, fp); fclose(fp); }
                memset(&e, 0, sizeof e);
                e.magic = STAGEREC_MAGIC; memcpy(e.key, v->key, 16); e.fo = v->fo; e.dep = v->dep; e.tex = v->tex_units; e.vh = v->h;
                snprintf(path, sizeof path, "%s/stage/%d_%016llx.var", vkc_dir(), s, (unsigned long long)v->hb);
                if ((fp = fopen(path, "ab")) != NULL) { fwrite(&e, 1, sizeof e, fp); fclose(fp); }
            no_record:;
            }
        }
    }
    if (v->ok && v->mod && modrel_on() == 2) { stage_mod_get(v); stage_mod_releasable(v); stage_mod_put(v); }
    EnterCriticalSection(&g_sh_lock);
    if (v->ok) { v->next_h = g_vh[v->h & 4095]; g_vh[v->h & 4095] = v; }
    v->state = 2;
    WakeAllConditionVariable(&g_sh_cv);
    LeaveCriticalSection(&g_sh_lock);
}
/* Any thread: the variant of this code for (key, fo). *how: 0 it was there, 1 made by this call, 2 waited for the thread
 * that was making it. */
static void workers_boost(void);
static VStage *stage_ensure(const SSrc *src, uint64_t hb, const uint32_t *key, uint32_t fo, int *how) {
    VStage *v, *busy;
    *how = 0;
    EnterCriticalSection(&g_sh_lock);
    for (;;) {
        v = vstage_find(hb, src->stage, src->code_len, key, fo, &busy);
        if (v && v->state == 2) break;
        if (!v) v = busy;
        if (v) { if (!*how) { *how = 2; workers_boost(); } while (v->state < 2) WD_SLEEP(&g_sh_cv, &g_sh_lock, INFINITE); continue; }
        v = vstage_new(hb, src->stage, src->code_len, key, fo);
        LeaveCriticalSection(&g_sh_lock);
        vstage_build(v, src);
        *how = 1;
        EnterCriticalSection(&g_sh_lock);
    }
    LeaveCriticalSection(&g_sh_lock);
    return v;
}
/* Render thread, at a draw or dispatch. */
/* ---- shader_wait = skip (graphics.ini; SMO_VK_NOHOLD=1 / =0 wins). What emulators call asynchronous shader
 * compilation: near a scene (scene_recent) a draw whose stage or pipeline is not ready is NOT waited for: the draw is left
 * out, a worker makes what was missing, and the draw appears in a later frame. Every draw left out is counted
 * ("nvn_vk_skip:" lines, "nvn_vk_skipf:" per frame, total at exit). Compute stages are never skipped. Off by
 * default: the picture is wrong for the frames in which something is left out. */
static int skip_on(void) { static int on = -1; if (on < 0) { const char *e = getenv("SMO_VK_NOHOLD"); on = e ? atoi(e) != 0 : gfx_options()->shader_skip; } return on; }
static int scene_recent(void);
static int skip_active(void) { return skip_on() && scene_recent(); }
static LONG64 g_skip_n, g_skip_frames, g_skip_fr_n, g_skip_stage, g_skip_pipe;
static int g_skip_flag;                            /* set by vstage_get when it returned NULL because the stage is being made */
static void skip_queue_stage(const SSrc *src, const uint32_t *key, uint32_t fo);
static void skip_note(int what, uint64_t vh, uint64_t fh) {
    g_skip_n++; g_skip_fr_n++; if (what) g_skip_pipe++; else g_skip_stage++;
    { static int nl; if (nl++ < 600) printf("nvn_vk_skip: frame %lld: a draw left out, its %s was not ready: vertex code %016llx, fragment code %016llx\n", (long long)nvn_front_frame_next(), what ? "pipeline (stage library or link)" : "stage", (unsigned long long)vh, (unsigned long long)fh); }
}
static LONG64 g_vstage_slow;          /* vstage_get calls that missed the stage's last variant (ssrc_hb over the code, the table under g_sh_lock) */
static VStage *vstage_get(Prog *p, int s, const uint32_t *key, uint32_t fo, int gprim) {      /* gprim: a geometry stage's primitive class (0 for the others) */
    Stage *st = &p->st[s];
    VStage *v = st->vk;
    SSrc src;
    int how;
    if (!st->code) return NULL;
    if (v && v->state == 2 && v->fo == fo && v->gprim == gprim && key_fits(s, key, v->key, v->dep, v->tex_units)) return v->ok ? v : NULL;
    g_vstage_slow++;
    if (!ssrc_of(&src, p, s)) return NULL;
    src.gprim = gprim;
    if (s != 5 && skip_active()) {                 /* shader_wait = skip: only what is ready */
        VStage *busy = NULL;
        uint64_t hb = ssrc_hb(&src);
        EnterCriticalSection(&g_sh_lock);
        v = vstage_find(hb, src.stage, src.code_len, key, fo, &busy);
        int ready = v && v->state == 2;
        LeaveCriticalSection(&g_sh_lock);
        if (!ready) { if (!v && !busy) skip_queue_stage(&src, key, fo); g_skip_flag = 1; skip_note(0, s ? 0 : hb, s ? hb : 0); return NULL; }
    }
    LONG64 t0 = tk();
    v = stage_ensure(&src, ssrc_hb(&src), key, fo, &how);
    if (how) { LONG64 dt = tk() - t0; hold_note(how == 1 ? H_STAGE_DRAW : H_STAGE_WAIT, dt); g_tm[6] += dt;
        if (holdlog_on()) { LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
            printf("nvn_vk_hold: frame %lld (%s): %s STAGE %s, %.2f ms: program #%d, code %016llx, key asked %x %x %x %x (kept %x %x %x %x), forced outputs %x; %s\n", (long long)nvn_front_frame_next(), g_in_scene ? "scene" : "no scene",
                   s == 5 ? "compute" : s == 2 ? "geometry" : s ? "fragment" : "vertex", how == 1 ? "made at the draw" : "waited for (a worker was making it)", (double)dt * 1000.0 / (double)fq.QuadPart, p->serial, (unsigned long long)v->hb,
                   key[0], key[1], key[2], key[3], v->key[0], v->key[1], v->key[2], v->key[3], fo, v->from_rec ? "from its record on disk" : "translated now (no record)");
            EnterCriticalSection(&g_sh_lock);
            printf("nvn_vk_hold:     %s; jobs waiting now: %lld foreground, %lld background\n", ho_text(v->hb), (long long)(g_jq_queued - g_jq_done), (long long)(g_bq_queued - g_bq_done));
            LeaveCriticalSection(&g_sh_lock); } }
    st->vk = v; memcpy(v->local, st->local, sizeof v->local);
    return v->ok ? v : NULL;
}

/* ---------------------------------------------------------------- pipelines */
typedef struct { uint32_t vs, fs, chanmask, sample_mask; int32_t cfmt[8], dfmt; uint8_t topo_class, ncolor, samples, poly, a2c, dclamp, logic, sshade, blend_on, pad[3]; uint8_t blend[8][8]; } PKey;
static uint64_t pkey_hash(const PKey *k) { return hmix(0xcbf29ce484222325ULL, k, sizeof *k); }      /* the table of pipelines (g_pipes, in memory only) */
typedef struct Pipe { PKey k; VkPipeline p, p_fast; /* p_fast: the pipeline linked at once, after the optimised one took its place */ VkPipelineLayout layout; VkDescriptorSetLayout dl[2]; int nb[2], push0; VStage *vs, *fs, *gs /* see gs_pipe_get */; volatile LONG state; uint8_t recorded, predicted, used, libbed, lib_how; struct Pipe *next; } Pipe;      /* libbed: linked from libraries (layout and set layouts belong to the pair library); lib_how: 0 the pair library was there, 1 compiled by this build, 2 waited for */      /* state: 0 queued for a worker, 1 being made, 2 made */
static Pipe *g_pipes[1024];
static VkShaderModule g_gs_dbg_fs;                 /* SMO_VK_GS_DEBUG=3 (gs_pipe_get): the fragment stage of draws with a geometry stage */
/* debug aid SMO_VK_GS_DEBUG=<mode> for the draws with a geometry stage (the lightning): 1 measures them (nvn_vk_draw.c
 * gsq_begin, gs_dbg_read: "nvn_vk_gs:" lines), 2 leaves them out (to compare pictures), 3 draws them in bright magenta (HDR
 * 200) without the depth test, and measures them as 1 does. */
static int gs_dbg_mode(void) { static int m = -1; if (m < 0) m = getenv("SMO_VK_GS_DEBUG") ? atoi(getenv("SMO_VK_GS_DEBUG")) : 0; return m; }
static void pkey_targets(PKey *k);
static int pkey_targets_of(PKey *k, int ncol, const TargetRef *col, const TargetRef *depth);
static VkBlendFactor blend_factor(unsigned f) {
    static const uint8_t m[9] = {VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
                                 VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_SRC_ALPHA_SATURATE};
    if (f >= 3 && f <= 0xB) return (VkBlendFactor)m[f - 3];
    switch (f) { case 1: return VK_BLEND_FACTOR_ZERO; case 2: return VK_BLEND_FACTOR_ONE; case 0x10: return VK_BLEND_FACTOR_SRC1_COLOR; case 0x11: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR; case 0x12: return VK_BLEND_FACTOR_SRC1_ALPHA;
                 case 0x13: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA; case 0x61: return VK_BLEND_FACTOR_CONSTANT_COLOR; case 0x62: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR; case 0x63: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
                 case 0x64: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA; }
    return VK_BLEND_FACTOR_ONE;
}
static VkBlendOp blend_op(unsigned e) { static const uint8_t m[6] = {VK_BLEND_OP_ADD, VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_REVERSE_SUBTRACT, VK_BLEND_OP_MIN, VK_BLEND_OP_MAX}; return (VkBlendOp)m[e < 6 ? e : 1]; }
static void layout_bindings(VStage *v, int s, VkDescriptorSetLayoutBinding *b0, int *n0, VkDescriptorSetLayoutBinding *b1, int *n1) {
    VkFlags sf = s == 2 ? VK_SHADER_STAGE_GEOMETRY_BIT : s ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
    for (unsigned n = 1; n < 18; n++) if (v->cbufs >> n & 1) { VkDescriptorSetLayoutBinding b = {40u * (unsigned)s + n, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, sf, NULL}; b0[(*n0)++] = b; }
    for (unsigned k = 0; k < 16; k++) if (v->ssbos >> k & 1) { VkDescriptorSetLayoutBinding b = {40u * (unsigned)s + 20 + k, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, sf, NULL}; b0[(*n0)++] = b; }
    for (int i = 0; i < v->nsmp; i++) { VkDescriptorSetLayoutBinding b = {v->smp[i].binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, sf, NULL}; b1[(*n1)++] = b; }
}
static Pipe *pipe_find(const PKey *k, uint64_t h) { for (Pipe *p = g_pipes[h & 1023]; p; p = p->next) if (!memcmp(&p->k, k, sizeof *k)) return p; return NULL; }      /* g_sh_lock held */
static Pipe *pipe_new(const PKey *k, VStage *vs, VStage *fs, uint64_t h, int state) {                                                                                 /* g_sh_lock held */
    Pipe *p = calloc(1, sizeof *p);
    p->k = *k; p->vs = vs; p->fs = fs; p->state = state;
    p->next = g_pipes[h & 1023]; g_pipes[h & 1023] = p;
    return p;
}
/* ---- graphics pipeline libraries (VK_EXT_graphics_pipeline_library; D.has_gpl, SMO_VK_GPL=1).
 * What a first run did not know at loading is (a) the pass formats a stage pair will be drawn with and (b) the topology
 * class: everything else of the key is dynamic state. With libraries the expensive part, the two shader
 * stages, is compiled WITHOUT those: one "pair library" per (vertex stage, fragment stage, samples, sample shading) holds
 * the pre-rasterization and the fragment shader state; it is made by the workers when the game hands the program over
 * (job_prog). The formats live in a "fragment output" library per pass signature and the topology class in a "vertex input"
 * library (three of them): both hold no shader and are made in microseconds. A draw LINKS the three (no compilation).
 * Not covered: a pair nobody announced (a fragment stage bound with the vertex stage of another program): its pair
 * library is compiled at the draw as the whole pipeline was before (counted: "pair library compiled at a draw"). */
typedef struct PairLib { VStage *vs, *fs; uint8_t samples, sshade; volatile LONG state; VkPipeline lib; VkPipelineLayout layout; VkDescriptorSetLayout dl[2]; int nb[2], push0; struct PairLib *next; } PairLib;
static PairLib *g_plib[1024];
static CRITICAL_SECTION g_gpl_lock;
static volatile LONG g_in_scene;
static struct { PKey k; VkPipeline lib; } g_olib[128];
static int g_nolib;
static VkPipeline g_vilib[3];
static LONG64 g_gpl_made, g_gpl_made_tk, g_gpl_draw, g_gpl_draw_tk, g_gpl_draw_max, g_gpl_wait, g_gpl_wait_tk, g_gpl_link, g_gpl_link_tk, g_gpl_link_max, g_gpl_dlink, g_gpl_dlink_tk, g_gpl_dlink_max, g_gpl_dlink_scene;
static DWORD g_render_tid;
static uint32_t pipe_dyn_list(VkDynamicState *dyn) {
    static const VkDynamicState base[20] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_LINE_WIDTH, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
        VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE, VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE,
        VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE, VK_DYNAMIC_STATE_VERTEX_INPUT_EXT};
    uint32_t ndyn = 20;
    memcpy(dyn, base, sizeof base);
    if (D.has_eds3) {
        dyn[ndyn++] = VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT; dyn[ndyn++] = VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT; dyn[ndyn++] = VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT; dyn[ndyn++] = VK_DYNAMIC_STATE_POLYGON_MODE_EXT;
        dyn[ndyn++] = VK_DYNAMIC_STATE_ALPHA_TO_COVERAGE_ENABLE_EXT; dyn[ndyn++] = VK_DYNAMIC_STATE_SAMPLE_MASK_EXT;
        if (D.f2.features.depthClamp) dyn[ndyn++] = VK_DYNAMIC_STATE_DEPTH_CLAMP_ENABLE_EXT;
    }
    return ndyn;
}
static void gpl_ms(VkPipelineMultisampleStateCreateInfo *ms, VkSampleMask *smask, int samples, int sshade) {      /* the same in the pair library and in the fragment output library */
    memset(ms, 0, sizeof *ms);
    ms->sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    *smask = 0xffffffffu;
    ms->rasterizationSamples = (VkSampleCountFlagBits)(samples ? samples : 1); ms->pSampleMask = smask;
    if (sshade == 1 && D.f2.features.sampleRateShading) { ms->sampleShadingEnable = VK_TRUE; ms->minSampleShading = 1.0f; }
}
/* Any thread, no lock held: the pair library; *how: 0 it was there, 1 compiled by this call, 2 waited for the thread compiling it. */
static PairLib *pairlib_ensure(VStage *vs, VStage *fs, int samples, int sshade, int *how) {
    unsigned h = (vs->id * 2654435761u ^ (fs ? fs->id * 40503u : 0) ^ (unsigned)(samples * 131 + sshade)) & 1023;
    PairLib *l;
    int mine = 0;
    LONG64 t0 = tk();
    *how = 0;
    EnterCriticalSection(&g_sh_lock);
    for (l = g_plib[h]; l; l = l->next) if (l->vs == vs && l->fs == fs && l->samples == samples && l->sshade == sshade) break;
    if (!l) { l = calloc(1, sizeof *l); l->vs = vs; l->fs = fs; l->samples = (uint8_t)samples; l->sshade = (uint8_t)sshade; l->state = 1; l->next = g_plib[h]; g_plib[h] = l; mine = 1; }
    else if (l->state == 1) { *how = 2; while (l->state < 2) WD_SLEEP(&g_sh_cv, &g_sh_lock, INFINITE); }
    LeaveCriticalSection(&g_sh_lock);
    if (!mine) { if (*how == 2) { InterlockedIncrement64(&g_gpl_wait); InterlockedAdd64(&g_gpl_wait_tk, tk() - t0); } return l; }
    *how = 1;
    VkDescriptorSetLayoutBinding b0[80], b1[140];
    layout_bindings(vs, 0, b0, &l->nb[0], b1, &l->nb[1]);
    if (fs) layout_bindings(fs, 1, b0, &l->nb[0], b1, &l->nb[1]);
    for (int i = 0; i < 2; i++) {
        VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = (uint32_t)l->nb[i]; li.pBindings = i ? b1 : b0;
        if (i == 0 && D.has_push && l->nb[0] <= 32) { li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR; l->push0 = 1; }
        VKC(vkCreateDescriptorSetLayout(V.device, &li, NULL, &l->dl[i]));
    }
    VkPushConstantRange pc = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
    VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 2; pl.pSetLayouts = l->dl; pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pc;
    VKC(vkCreatePipelineLayout(V.device, &pl, NULL, &l->layout));
    VkPipelineShaderStageCreateInfo st[2];
    memset(st, 0, sizeof st);
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = stage_mod_get(vs); st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs ? stage_mod_get(fs) : NULL; st[1].pName = "main";
    stage_warp32(&st[0], vs); stage_warp32(&st[1], fs);
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1; vp.scissorCount = 1; vp.pNext = vk_vp_next();
    VkPipelineRasterizationProvokingVertexStateCreateInfoEXT pv = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT};
    pv.provokingVertexMode = VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT;
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.lineWidth = 1.0f;
    if (D.has_pv) rs.pNext = &pv;
    VkPipelineMultisampleStateCreateInfo ms; VkSampleMask smask;
    gpl_ms(&ms, &smask, samples, sshade);
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.maxDepthBounds = 1.0f;
    VkDynamicState dyn[32];
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = pipe_dyn_list(dyn); dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    VkGraphicsPipelineLibraryCreateInfoEXT gl = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT};
    gl.pNext = &ri; gl.flags = VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT | VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT;
    VkGraphicsPipelineCreateInfo gi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gi.pNext = &gl; gi.flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR | VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT; gi.stageCount = fs ? 2 : 1; gi.pStages = st; gi.pViewportState = &vp; gi.pRasterizationState = &rs; gi.pMultisampleState = &ms;
    gi.pDepthStencilState = &ds; gi.pDynamicState = &dy; gi.layout = l->layout;
    VkResult r = vkCreateGraphicsPipelines(V.device, g_pcache, 1, &gi, NULL, &l->lib);
    stage_mod_put(vs); if (fs) stage_mod_put(fs);
    if (r != VK_SUCCESS) vkd_fail("vkCreateGraphicsPipelines, pair library (vertex stage %u, fragment stage %u): %s", vs->id, fs ? fs->id : 0, vk_result_name(r));
    LONG64 dt = tk() - t0;
    EnterCriticalSection(&g_sh_lock);
    l->state = 2;
    WakeAllConditionVariable(&g_sh_cv);
    LeaveCriticalSection(&g_sh_lock);
    if (GetCurrentThreadId() == g_render_tid) {    /* compiled at a draw: the hold the libraries are there to remove */
        g_gpl_draw++; g_gpl_draw_tk += dt; if (dt > g_gpl_draw_max) g_gpl_draw_max = dt;
        if (holdlog_on()) { LARGE_INTEGER fq; QueryPerformanceFrequency(&fq); printf("nvn_vk_hold: frame %lld (%s): PAIR LIBRARY compiled at the draw, %.2f ms: vertex code %016llx (forced outputs %x), fragment code %016llx (%s)\n", (long long)nvn_front_frame_next(), g_in_scene ? "scene" : "no scene",
                                   (double)dt * 1000.0 / (double)fq.QuadPart, (unsigned long long)vs->hb, vs->fo, (unsigned long long)(fs ? fs->hb : 0), fs && fs->fragonly ? "a fragment stage of a program without a vertex stage" : "a program with both stages, or a pair the look-ahead met"); }
    } else { InterlockedIncrement64(&g_gpl_made); InterlockedAdd64(&g_gpl_made_tk, dt); }
    return l;
}
static VkPipeline gpl_vilib(int topo_class) {
    EnterCriticalSection(&g_gpl_lock);
    if (!g_vilib[topo_class]) {
        VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = topo_class == 0 ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST : topo_class == 1 ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkDynamicState dyn[32];
        VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dy.dynamicStateCount = pipe_dyn_list(dyn); dy.pDynamicStates = dyn;
        VkGraphicsPipelineLibraryCreateInfoEXT gl = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT};
        gl.flags = VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT;
        VkGraphicsPipelineCreateInfo gi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gi.pNext = &gl; gi.flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR | VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT; gi.pInputAssemblyState = &ia; gi.pDynamicState = &dy;
        VKC(vkCreateGraphicsPipelines(V.device, NULL, 1, &gi, NULL, &g_vilib[topo_class]));
    }
    VkPipeline p = g_vilib[topo_class];
    LeaveCriticalSection(&g_gpl_lock);
    return p;
}
static VkPipeline gpl_olib(const PKey *k0) {
    PKey k = *k0;
    k.vs = k.fs = 0; k.topo_class = 0;
    EnterCriticalSection(&g_gpl_lock);
    int i;
    for (i = 0; i < g_nolib; i++) if (!memcmp(&g_olib[i].k, &k, sizeof k)) break;
    if (i == g_nolib) {
        if (g_nolib == 128) vkd_fail("more than 128 pass signatures (fragment output libraries)");
        VkPipelineMultisampleStateCreateInfo ms; VkSampleMask smask;
        gpl_ms(&ms, &smask, k.samples, k.sshade);
        VkPipelineColorBlendAttachmentState ba[8];
        memset(ba, 0, sizeof ba);
        for (int c = 0; c < k.ncolor; c++) { ba[c].colorWriteMask = 15; ba[c].srcColorBlendFactor = ba[c].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; ba[c].dstColorBlendFactor = ba[c].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE; }      /* dynamic (extended dynamic state 3) */
        VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = k.ncolor; cb.pAttachments = ba;
        if (k.logic != 3 && D.f2.features.logicOp) { cb.logicOpEnable = VK_TRUE; cb.logicOp = (VkLogicOp)(k.logic & 15); }
        VkDynamicState dyn[32];
        VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dy.dynamicStateCount = pipe_dyn_list(dyn); dy.pDynamicStates = dyn;
        VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        VkFormat cf[8];
        for (int c = 0; c < 8; c++) cf[c] = (VkFormat)k.cfmt[c];
        ri.colorAttachmentCount = k.ncolor; ri.pColorAttachmentFormats = cf;
        if (fmt_aspect((VkFormat)k.dfmt) & VK_IMAGE_ASPECT_DEPTH_BIT) ri.depthAttachmentFormat = (VkFormat)k.dfmt;
        if (k.dfmt && (fmt_aspect((VkFormat)k.dfmt) & VK_IMAGE_ASPECT_STENCIL_BIT)) ri.stencilAttachmentFormat = (VkFormat)k.dfmt;
        VkGraphicsPipelineLibraryCreateInfoEXT gl = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT};
        gl.pNext = &ri; gl.flags = VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT;
        VkGraphicsPipelineCreateInfo gi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gi.pNext = &gl; gi.flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR | VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT; gi.pMultisampleState = &ms; gi.pColorBlendState = &cb; gi.pDynamicState = &dy;
        VKC(vkCreateGraphicsPipelines(V.device, NULL, 1, &gi, NULL, &g_olib[i].lib));
        g_olib[i].k = k; g_nolib++;
    }
    VkPipeline p = g_olib[i].lib;
    LeaveCriticalSection(&g_gpl_lock);
    return p;
}
/* ---- SMO_VK_GPL=2: one library per STAGE instead of per pair, so that no pair has to be known before its draw (a fragment
 * stage of a program without a vertex stage is drawn with the vertex stage of another program: 24 of the 30 libraries still
 * compiled at a draw). Two libraries of one pipeline must agree on every descriptor set layout both use, and our
 * sets hold the bindings of both stages; so every stage library is made with ONE layout that declares every binding a vertex
 * or a fragment stage can have (set 0: 66 buffers, set 1: 128 samplers; a declared binding a shader does not use needs no
 * descriptor). The price: set 0 is too large for push descriptors (32), so a draw allocates and writes a set instead.
 * A draw links: vertex input + vertex stage library + fragment stage library + fragment output. A vertex stage drawn with no
 * fragment stage has a second library holding the (empty) fragment shader state as well. Sample shading (never met:
 * 0 of 1,380 recorded keys) takes the pair library path. */
static VkDescriptorSetLayout g_udl[2];
static VkPipelineLayout g_ulayout;
static int gpl_sep(void) { static int m = -1; if (m < 0) m = D.has_gpl && vk_gpl_mode() >= 2; return m; }
static void gpl_universal(void) {
    EnterCriticalSection(&g_gpl_lock);
    if (!g_ulayout) {
        static VkDescriptorSetLayoutBinding b0[66], b1[128];
        int n0 = 0, n1 = 0;
        for (unsigned s = 0; s < 2; s++) {
            VkFlags sf = s ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
            for (unsigned n = 1; n < 18; n++) { VkDescriptorSetLayoutBinding b = {40u * s + n, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, sf, NULL}; b0[n0++] = b; }
            for (unsigned k = 0; k < 16; k++) { VkDescriptorSetLayoutBinding b = {40u * s + 20 + k, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, sf, NULL}; b0[n0++] = b; }
            for (unsigned u = 0; u < 32; u++) { VkDescriptorSetLayoutBinding b = {40u * s + u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, sf, NULL}; b1[n1++] = b; b.binding += 240; b1[n1++] = b; }
        }
        for (int i = 0; i < 2; i++) {
            VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            li.bindingCount = (uint32_t)(i ? n1 : n0); li.pBindings = i ? b1 : b0;
            VKC(vkCreateDescriptorSetLayout(V.device, &li, NULL, &g_udl[i]));
        }
        VkPushConstantRange pc = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
        VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 2; pl.pSetLayouts = g_udl; pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pc;
        VKC(vkCreatePipelineLayout(V.device, &pl, NULL, &g_ulayout));
    }
    LeaveCriticalSection(&g_gpl_lock);
}
static int gpl_opt_on(void);
static int stage_fits_universal(const VStage *v) { for (int i = 0; i < v->nsmp; i++) { unsigned b = v->smp[i].binding; if (b >= 240) b -= 240; if (b >= 80 || b % 40u >= 32) return 0; } return 1; }      /* texture units 0..31 only */
/* Any thread, no lock held: the library of one stage (alone: a vertex stage that is drawn without a fragment stage). */
static VkPipeline stagelib_ensure(VStage *v, int alone, int *how) {
    VkPipeline *lib = alone ? &v->lib2 : &v->lib;
    volatile LONG *state = alone ? &v->lib2_state : &v->lib_state;
    int mine = 0;
    LONG64 t0 = tk();
    *how = 0;
    gpl_universal();
    EnterCriticalSection(&g_sh_lock);
    if (*state == 0) { *state = 1; mine = 1; }
    else if (*state == 1) { *how = 2; workers_boost(); while (*state < 2) WD_SLEEP(&g_sh_cv, &g_sh_lock, INFINITE); }
    LeaveCriticalSection(&g_sh_lock);
    if (!mine) { if (*how == 2) { InterlockedIncrement64(&g_gpl_wait); InterlockedAdd64(&g_gpl_wait_tk, tk() - t0); } return *lib; }
    *how = 1;
    int isfs = v->stage == 1;
    VkPipelineShaderStageCreateInfo st = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    st.stage = isfs ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT; st.module = stage_mod_get(v); st.pName = "main";
    stage_warp32(&st, v);
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1; vp.scissorCount = 1; vp.pNext = vk_vp_next();
    VkPipelineRasterizationProvokingVertexStateCreateInfoEXT pv = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT};
    pv.provokingVertexMode = VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT;
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.lineWidth = 1.0f;
    if (D.has_pv) rs.pNext = &pv;
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.maxDepthBounds = 1.0f;
    VkDynamicState dyn[32];
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = pipe_dyn_list(dyn); dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    VkGraphicsPipelineLibraryCreateInfoEXT gl = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT};
    gl.pNext = &ri; gl.flags = isfs ? VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT : VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT | (alone ? VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT : 0);
    VkGraphicsPipelineCreateInfo gi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    /* the link-time-optimisation information is kept only when something will ask for it (SMO_VK_GPL_OPT=1) */
    gi.pNext = &gl; gi.flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR | (gpl_opt_on() ? VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT : 0); gi.stageCount = 1; gi.pStages = &st; gi.pDynamicState = &dy; gi.layout = g_ulayout;
    if (!isfs) { gi.pViewportState = &vp; gi.pRasterizationState = &rs; }
    if (isfs || alone) gi.pDepthStencilState = &ds;
    {   /* SMO measurement switch SMO_VK_NOOPT_BYTES=<n>: a stage of more than n bytes of machine code is compiled by the driver
         * without optimisation (one SMO fragment stage of the opening, 1.7 MB of GLSL, kept the NVIDIA compiler busy > 40 s) */
        static long long lim = -2;
        if (lim == -2) { const char *e = getenv("SMO_VK_NOOPT_BYTES"); lim = e ? atoll(e) : -1; }
        if (lim >= 0 && v->code_len > lim) gi.flags |= VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT;
    }
    LONG64 tc0 = tk();
    VkResult r = vkCreateGraphicsPipelines(V.device, g_pcache, 1, &gi, NULL, lib);
    if (r != VK_SUCCESS) vkd_fail("vkCreateGraphicsPipelines, stage library (%s stage %u): %s", isfs ? "fragment" : "vertex", v->id, vk_result_name(r));
    if (!alone) stage_mod_releasable(v);
    stage_mod_put(v);
    {   LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
        double sec = (double)(tk() - tc0) / (double)fq.QuadPart;
        if (sec > 1.0) printf("nvn_vk_gpl: stage library (%s stage %u, %d bytes of machine code%s) took %.1f s in the driver\n", isfs ? "fragment" : "vertex", v->id, v->code_len, (gi.flags & VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT) ? ", NOT optimised" : "", sec);
    }
    LONG64 dt = tk() - t0;
    EnterCriticalSection(&g_sh_lock);
    *state = 2;
    WakeAllConditionVariable(&g_sh_cv);
    LeaveCriticalSection(&g_sh_lock);
    if (GetCurrentThreadId() == g_render_tid) {
        g_gpl_draw++; g_gpl_draw_tk += dt; if (dt > g_gpl_draw_max) g_gpl_draw_max = dt;
        if (holdlog_on()) { LARGE_INTEGER fq; QueryPerformanceFrequency(&fq); printf("nvn_vk_hold: frame %lld (%s): STAGE LIBRARY compiled at the draw, %.2f ms: %s code %016llx (key %x %x %x %x, forced outputs %x)%s\n", (long long)nvn_front_frame_next(), g_in_scene ? "scene" : "no scene",
                                   (double)dt * 1000.0 / (double)fq.QuadPart, isfs ? "fragment" : "vertex", (unsigned long long)v->hb, v->key[0], v->key[1], v->key[2], v->key[3], v->fo, alone ? " (a vertex stage drawn without a fragment stage)" : ""); }
    } else { InterlockedIncrement64(&g_gpl_made); InterlockedAdd64(&g_gpl_made_tk, dt); }
    return *lib;
}
static void gpl_destroy(void) {
    for (int i = 0; i < 4096; i++) for (VStage *v = g_vstage[i]; v; v = v->next) { if (v->lib) { vkDestroyPipeline(V.device, v->lib, NULL); v->lib = NULL; } if (v->lib2) { vkDestroyPipeline(V.device, v->lib2, NULL); v->lib2 = NULL; } }
    if (g_ulayout) { vkDestroyPipelineLayout(V.device, g_ulayout, NULL); vkDestroyDescriptorSetLayout(V.device, g_udl[0], NULL); vkDestroyDescriptorSetLayout(V.device, g_udl[1], NULL); g_ulayout = NULL; }
    for (int i = 0; i < 1024; i++) for (PairLib *l = g_plib[i], *n; l; l = n) { n = l->next; if (l->lib) vkDestroyPipeline(V.device, l->lib, NULL); if (l->layout) vkDestroyPipelineLayout(V.device, l->layout, NULL); for (int k = 0; k < 2; k++) if (l->dl[k]) vkDestroyDescriptorSetLayout(V.device, l->dl[k], NULL); free(l); g_plib[i] = NULL; }
    for (int i = 0; i < g_nolib; i++) vkDestroyPipeline(V.device, g_olib[i].lib, NULL);
    g_nolib = 0;
    for (int i = 0; i < 3; i++) if (g_vilib[i]) { vkDestroyPipeline(V.device, g_vilib[i], NULL); g_vilib[i] = NULL; }
}
/* A pipeline linked at once from libraries is not optimised across its stages (measured by ssbu-recomp in Super Smash Bros. Ultimate on
 * NVIDIA: 3.3 ms of GPU time per frame against 1.6 ms). So every pipeline linked from stage libraries is linked AGAIN by a worker with
 * VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION (a full compilation, background queue) and the handle is exchanged; the first
 * one stays alive until shutdown (command buffers in flight use it). SMO_VK_GPL_OPT=0 turns this off. */
static void gpl_opt_push(struct Pipe *p);          /* g_sh_lock held (defined after the job queue) */
static LONG64 g_gpl_opt_q, g_gpl_opt_n, g_gpl_opt_tk;
static int g_workers_quit;
/* OFF by default. Measured by ssbu-recomp in Super Smash Bros. Ultimate (first runs): it cost 195 s (NVIDIA) and 304 s
 * (Intel) of worker time in one run, the GPU time per frame did not change (NVIDIA 3.23 against 3.34 ms, Intel 8.60
 * against 8.80) and the holds went UP (16 / 744 ms against 13 / 732 ms; Intel 53 / 2,055 ms against 37 / 1,199 ms). A second
 * run has the normal GPU time without it (1.59 ms), so the higher first-run number is not the missing optimisation. */
static int gpl_opt_on(void) { static int on = -1; if (on < 0) on = getenv("SMO_VK_GPL_OPT") && atoi(getenv("SMO_VK_GPL_OPT")); return on; }
static void gpl_optimise(Pipe *p) {                /* a worker */
    const PKey *k = &p->k;
    LONG64 t0 = tk();
    int n = 0;
    VkPipeline libs[4], opt = NULL;
    libs[n++] = gpl_vilib(k->topo_class < 3 ? k->topo_class : 2);
    libs[n++] = p->fs ? p->vs->lib : p->vs->lib2;
    if (p->fs) libs[n++] = p->fs->lib;
    libs[n++] = gpl_olib(k);
    for (int i = 0; i < n; i++) if (!libs[i]) return;
    VkPipelineLibraryCreateInfoKHR pli = {VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR};
    pli.libraryCount = (uint32_t)n; pli.pLibraries = libs;
    VkGraphicsPipelineCreateInfo gi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gi.pNext = &pli; gi.layout = g_ulayout; gi.flags = VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT;
    if (vkCreateGraphicsPipelines(V.device, g_pcache, 1, &gi, NULL, &opt) != VK_SUCCESS || !opt) return;
    p->p_fast = InterlockedExchangePointer((void *volatile *)&p->p, opt);
    InterlockedIncrement64(&g_gpl_opt_n); InterlockedAdd64(&g_gpl_opt_tk, tk() - t0);
}
/* Makes the pipeline of p->k (any thread, no lock: the device calls are thread-safe, the pipeline cache is synchronised by
 * the driver), then marks it made. Returns the milliseconds it took. */
static double pipe_build(Pipe *p) {
    const PKey *k = &p->k;
    VStage *vs = p->vs, *fs = p->fs;
    LONG64 t0 = tk();
    if (D.has_gpl && !p->gs && gpl_sep() && !k->sshade && stage_fits_universal(vs) && (!fs || stage_fits_universal(fs))) {      /* SMO_VK_GPL=2: linked from one library per stage */
        int how, how2 = 0, n = 0;
        VkPipeline libs[4];
        libs[n++] = gpl_vilib(k->topo_class < 3 ? k->topo_class : 2);
        libs[n++] = stagelib_ensure(vs, !fs, &how);
        if (fs) libs[n++] = stagelib_ensure(fs, 0, &how2);
        libs[n++] = gpl_olib(k);
        if (how2 == 1 || how == 1) how = 1; else if (how2 == 2 || how == 2) how = 2;
        LONG64 t1 = tk();
        VkPipelineLibraryCreateInfoKHR pli = {VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR};
        pli.libraryCount = (uint32_t)n; pli.pLibraries = libs;
        VkGraphicsPipelineCreateInfo gi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gi.pNext = &pli; gi.layout = g_ulayout;
        VkResult r = vkCreateGraphicsPipelines(V.device, g_pcache, 1, &gi, NULL, &p->p);
        if (r != VK_SUCCESS) vkd_fail("vkCreateGraphicsPipelines, link of stage libraries (vertex stage %u, fragment stage %u, %d colour targets, depth format %d): %s", k->vs, k->fs, k->ncolor, k->dfmt, vk_result_name(r));
        LONG64 dt = tk() - t1;
        p->layout = g_ulayout; p->dl[0] = g_udl[0]; p->dl[1] = g_udl[1]; p->nb[0] = 66; p->nb[1] = 128; p->push0 = 0; p->libbed = 1; p->lib_how = (uint8_t)how;
        InterlockedIncrement64(&R.pipelines);
        if (GetCurrentThreadId() == g_render_tid) { g_gpl_dlink++; g_gpl_dlink_tk += dt; if (dt > g_gpl_dlink_max) g_gpl_dlink_max = dt; if (g_in_scene) g_gpl_dlink_scene++; }
        else { InterlockedIncrement64(&g_gpl_link); InterlockedAdd64(&g_gpl_link_tk, dt); }
        EnterCriticalSection(&g_sh_lock);
        p->state = 2;
        if (gpl_opt_on() && !g_workers_quit) gpl_opt_push(p);      /* the same pipeline again, optimised across its stages, on a worker */
        WakeAllConditionVariable(&g_sh_cv);
        LeaveCriticalSection(&g_sh_lock);
        { LARGE_INTEGER fq; QueryPerformanceFrequency(&fq); return (double)(tk() - t0) * 1000.0 / (double)fq.QuadPart; }
    }
    if (D.has_gpl && !p->gs) {                     /* linked from libraries */
        int how;
        PairLib *l = pairlib_ensure(vs, fs, k->samples, k->sshade, &how);
        VkPipeline libs[3];
        libs[0] = gpl_vilib(k->topo_class < 3 ? k->topo_class : 2); libs[1] = l->lib; libs[2] = gpl_olib(k);
        LONG64 t1 = tk();
        VkPipelineLibraryCreateInfoKHR pli = {VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR};
        pli.libraryCount = 3; pli.pLibraries = libs;
        VkGraphicsPipelineCreateInfo gi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gi.pNext = &pli; gi.layout = l->layout;
        VkResult r = vkCreateGraphicsPipelines(V.device, g_pcache, 1, &gi, NULL, &p->p);
        if (r != VK_SUCCESS) vkd_fail("vkCreateGraphicsPipelines, link of libraries (vertex stage %u, fragment stage %u, %d colour targets, depth format %d): %s", k->vs, k->fs, k->ncolor, k->dfmt, vk_result_name(r));
        LONG64 dt = tk() - t1;
        p->layout = l->layout; p->dl[0] = l->dl[0]; p->dl[1] = l->dl[1]; p->nb[0] = l->nb[0]; p->nb[1] = l->nb[1]; p->push0 = l->push0; p->libbed = 1; p->lib_how = (uint8_t)how;
        InterlockedIncrement64(&R.pipelines);
        if (GetCurrentThreadId() == g_render_tid) { g_gpl_dlink++; g_gpl_dlink_tk += dt; if (dt > g_gpl_dlink_max) g_gpl_dlink_max = dt; if (g_in_scene) g_gpl_dlink_scene++; }
        else { InterlockedIncrement64(&g_gpl_link); InterlockedAdd64(&g_gpl_link_tk, dt); }
        EnterCriticalSection(&g_sh_lock);
        p->state = 2;
        WakeAllConditionVariable(&g_sh_cv);
        LeaveCriticalSection(&g_sh_lock);
        { LARGE_INTEGER fq; QueryPerformanceFrequency(&fq); return (double)(tk() - t0) * 1000.0 / (double)fq.QuadPart; }
    }
    VkDescriptorSetLayoutBinding b0[120], b1[200];
    layout_bindings(vs, 0, b0, &p->nb[0], b1, &p->nb[1]);
    if (fs) layout_bindings(fs, 1, b0, &p->nb[0], b1, &p->nb[1]);
    if (p->gs) layout_bindings(p->gs, 2, b0, &p->nb[0], b1, &p->nb[1]);
    for (int i = 0; i < 2; i++) {
        VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = (uint32_t)p->nb[i]; li.pBindings = i ? b1 : b0;
        if (i == 0 && D.has_push && p->nb[0] <= 32) { li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR; p->push0 = 1; }      /* 32: the smallest maxPushDescriptors a device may have */
        VKC(vkCreateDescriptorSetLayout(V.device, &li, NULL, &p->dl[i]));
    }
    VkPushConstantRange pc = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
    VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 2; pl.pSetLayouts = p->dl; pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pc;
    VKC(vkCreatePipelineLayout(V.device, &pl, NULL, &p->layout));

    VkPipelineShaderStageCreateInfo st[3];
    uint32_t nst = 0;
    memset(st, 0, sizeof st);
    st[0].sType = st[1].sType = st[2].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[nst].stage = VK_SHADER_STAGE_VERTEX_BIT; st[nst].module = stage_mod_get(vs); st[nst++].pName = "main";
    if (p->gs) { st[nst].stage = VK_SHADER_STAGE_GEOMETRY_BIT; st[nst].module = stage_mod_get(p->gs); st[nst++].pName = "main"; }
    if (fs) { st[nst].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[nst].module = stage_mod_get(fs); if (p->gs && g_gs_dbg_fs) st[nst].module = g_gs_dbg_fs; st[nst++].pName = "main"; }
    stage_warp32(&st[0], vs); if (p->gs) stage_warp32(&st[1], p->gs); if (fs && !(p->gs && g_gs_dbg_fs)) stage_warp32(&st[nst - 1], fs);
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = k->topo_class == 0 ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST : k->topo_class == 1 ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1; vp.scissorCount = 1; vp.pNext = vk_vp_next();
    VkPipelineRasterizationProvokingVertexStateCreateInfoEXT pv = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT};
    pv.provokingVertexMode = VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT;
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = D.has_eds3 ? VK_POLYGON_MODE_FILL : k->poly == 0 ? VK_POLYGON_MODE_POINT : k->poly == 1 ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL; rs.depthClampEnable = k->dclamp; rs.lineWidth = 1.0f;
    if (D.has_pv) rs.pNext = &pv;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    VkSampleMask smask = D.has_eds3 ? 0xffffffffu : k->sample_mask;
    ms.rasterizationSamples = (VkSampleCountFlagBits)(k->samples ? k->samples : 1); ms.alphaToCoverageEnable = k->a2c; ms.pSampleMask = &smask;
    if (k->sshade == 1 && D.f2.features.sampleRateShading) { ms.sampleShadingEnable = VK_TRUE; ms.minSampleShading = 1.0f; }
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.maxDepthBounds = 1.0f;
    VkPipelineColorBlendAttachmentState ba[8];
    memset(ba, 0, sizeof ba);
    for (int i = 0; i < k->ncolor; i++) {
        const uint8_t *b = k->blend[i];
        ba[i].blendEnable = k->blend_on >> i & 1; ba[i].colorWriteMask = D.has_eds3 ? 15 : (k->chanmask >> (i * 4)) & 15;
        ba[i].srcColorBlendFactor = blend_factor(b[0]); ba[i].dstColorBlendFactor = blend_factor(b[1]); ba[i].srcAlphaBlendFactor = blend_factor(b[2]); ba[i].dstAlphaBlendFactor = blend_factor(b[3]);
        ba[i].colorBlendOp = blend_op(b[4]); ba[i].alphaBlendOp = blend_op(b[5]);
    }
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = k->ncolor; cb.pAttachments = ba;
    if (k->logic != 3 && D.f2.features.logicOp) { cb.logicOpEnable = VK_TRUE; cb.logicOp = (VkLogicOp)(k->logic & 15); }
    VkDynamicState dyn[32] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_LINE_WIDTH, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
        VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE, VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE,
        VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE, VK_DYNAMIC_STATE_VERTEX_INPUT_EXT};
    uint32_t ndyn = 20;
    if (D.has_eds3) {
        dyn[ndyn++] = VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT; dyn[ndyn++] = VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT; dyn[ndyn++] = VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT; dyn[ndyn++] = VK_DYNAMIC_STATE_POLYGON_MODE_EXT;
        dyn[ndyn++] = VK_DYNAMIC_STATE_ALPHA_TO_COVERAGE_ENABLE_EXT; dyn[ndyn++] = VK_DYNAMIC_STATE_SAMPLE_MASK_EXT;
        if (D.f2.features.depthClamp) dyn[ndyn++] = VK_DYNAMIC_STATE_DEPTH_CLAMP_ENABLE_EXT;
    }
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = ndyn; dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    VkFormat cf[8];
    for (int i = 0; i < 8; i++) cf[i] = (VkFormat)k->cfmt[i];
    ri.colorAttachmentCount = k->ncolor; ri.pColorAttachmentFormats = cf;
    if (fmt_aspect((VkFormat)k->dfmt) & VK_IMAGE_ASPECT_DEPTH_BIT) ri.depthAttachmentFormat = (VkFormat)k->dfmt;
    if (k->dfmt && (fmt_aspect((VkFormat)k->dfmt) & VK_IMAGE_ASPECT_STENCIL_BIT)) ri.stencilAttachmentFormat = (VkFormat)k->dfmt;
    VkGraphicsPipelineCreateInfo gi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gi.pNext = &ri; gi.stageCount = nst; gi.pStages = st; gi.pInputAssemblyState = &ia; gi.pViewportState = &vp; gi.pRasterizationState = &rs; gi.pMultisampleState = &ms;
    gi.pDepthStencilState = &ds; gi.pColorBlendState = &cb; gi.pDynamicState = &dy; gi.layout = p->layout;
    VkResult r = vkCreateGraphicsPipelines(V.device, g_pcache, 1, &gi, NULL, &p->p);
    stage_mod_put(vs); if (p->gs) stage_mod_put(p->gs); if (fs) stage_mod_put(fs);
    if (r != VK_SUCCESS) vkd_fail("vkCreateGraphicsPipelines (vertex stage %u, fragment stage %u, geometry stage %u, %d colour targets, depth format %d): %s", k->vs, k->fs, p->gs ? p->gs->id : 0, k->ncolor, k->dfmt, vk_result_name(r));
    InterlockedIncrement64(&R.pipelines);
    EnterCriticalSection(&g_sh_lock);
    p->state = 2;
    WakeAllConditionVariable(&g_sh_cv);
    LeaveCriticalSection(&g_sh_lock);
    { LARGE_INTEGER fq; QueryPerformanceFrequency(&fq); return (double)(tk() - t0) * 1000.0 / (double)fq.QuadPart; }
}
/* ..\cache\vk\pipes.bin: every pipeline key a draw on this PC ever USED, with its stages named by their variant hashes.
 * Read at start by the workers. A predicted pipeline is written here only when a draw uses it. */
typedef struct { uint32_t magic, compute; uint64_t vs_h, fs_h; PKey k; } PipeRec;
#define PIPEREC_MAGIC (0x31455049u + MX_VK_VERSION)
static void piperec_append(const PKey *k, VStage *vs, VStage *fs, int compute) {
    PipeRec r;
    memset(&r, 0, sizeof r);
    r.magic = PIPEREC_MAGIC; r.compute = (uint32_t)compute; r.vs_h = vs->h; r.fs_h = fs ? fs->h : 0;
    if (k) { r.k = *k; r.k.vs = r.k.fs = 0; }
    FILE *fp = fopen(vkc_file("pipes.bin"), "ab");
    if (fp) { fwrite(&r, 1, sizeof r, fp); fclose(fp); }
}

/* ---- prediction: pass signatures seen so far, and the stage pairs the game handed over */
typedef struct { PKey k; int uses; } Sig;          /* k with vs = fs = 0, triangles, no sample shading */
typedef struct PairRec { VStage *vs, *fs; uint8_t drawn, npred, sshade; uint64_t sigs; struct PairRec *next, *all; } PairRec;
static Sig g_sig[64];
static int g_nsig, g_predict;
static PairRec *g_pair_tab[1024], *g_pair_all;
static PairRec *pair_get(VStage *vs, VStage *fs) {                 /* g_sh_lock held */
    unsigned h = (vs->id * 2654435761u ^ (fs ? fs->id * 40503u : 0)) & 1023;
    PairRec *r;
    for (r = g_pair_tab[h]; r; r = r->next) if (r->vs == vs && r->fs == fs) return r;
    r = calloc(1, sizeof *r);
    r->vs = vs; r->fs = fs; r->next = g_pair_tab[h]; g_pair_tab[h] = r; r->all = g_pair_all; g_pair_all = r;
    return r;
}
static int sig_note(const PKey *k, int *idx) {     /* g_sh_lock held: the index of the signature when it is NEW, else -1; *idx: its index either way (-1: table full) */
    PKey s = *k;
    s.vs = s.fs = 0; s.topo_class = 2; s.sshade = 0;
    for (int i = 0; i < g_nsig; i++) if (!memcmp(&g_sig[i].k, &s, sizeof s)) { g_sig[i].uses++; *idx = i; return -1; }
    *idx = -1;
    if (g_nsig == 64) return -1;
    g_sig[g_nsig].k = s; g_sig[g_nsig].uses = 1;
    *idx = g_nsig;
    return g_nsig++;
}
static void gpl_shared_vs(VStage *vs);
/* ---- the job queue of the workers */
typedef struct Job { struct Job *next; int kind, sshade, has[3], fragonly; /* kind 3: the fragment stage of a program without a vertex stage */ Pipe *pipe; VStage *lvs, *lfs; /* kind 5: the pair library of these two stages */ PKey k; uint32_t key0[4], key1[4]; SSrc src[3]; uint8_t data[1]; } Job;      /* kind: 0 a program handed over, 1 one pipeline, 2 the stages and the pipeline of a draw the look-ahead met, 3 one stage, 5 a pair library, 6 an optimised link, 7 the stage a skipped draw waits for */
/* debug aid: what each worker is doing now (printed when the loading barrier times out) */
static struct { volatile LONG kind; volatile LONG serial; volatile LONG64 t0; volatile DWORD tid; } g_wk[64];
static volatile LONG g_wk_n;
static Job *g_jq_head, *g_jq_tail, *g_bq_head, *g_bq_tail;      /* g_bq: the background queue (pair libraries without a program of their own, optimised links): taken only when g_jq is empty */
static LONG64 g_jq_queued, g_jq_done, g_bq_queued, g_bq_done;
static LONG64 g_boosts;                            /* times the render thread raised the workers' priority because it waited for one (workers_boost) */
static void job_push_bg(Job *j) {                  /* g_sh_lock held */
    j->next = NULL;
    if (g_bq_tail) g_bq_tail->next = j; else g_bq_head = j;
    g_bq_tail = j; g_bq_queued++;
    WakeConditionVariable(&g_jq_cv);
}
static void job_push_front(Job *j);
static void job_copy_src(Job *j, uint8_t **d, int i, const SSrc *s);
static void skip_queue_stage(const SSrc *src, const uint32_t *key, uint32_t fo) {      /* render thread, no lock held: the stage a skipped draw needs, to the front of the workers' queue (once) */
    static uint64_t asked[2048]; static int nasked;
    uint64_t id = fnv(fnv(ssrc_hb(src), key, 16), &fo, 4) ^ (uint64_t)src->stage;
    for (int i = 0; i < nasked; i++) if (asked[i] == id) return;
    if (nasked < 2048) asked[nasked++] = id;
    Job *j = calloc(1, sizeof *j + (size_t)src->code_len + src->cdata_size + 16);
    uint8_t *d = j->data;
    j->kind = 7; j->sshade = (int)fo; memcpy(j->key1, key, 16);
    job_copy_src(j, &d, 1, src);
    EnterCriticalSection(&g_sh_lock);
    job_push_front(j);
    LeaveCriticalSection(&g_sh_lock);
}
static void gpl_opt_push(Pipe *p) { Job *j = calloc(1, sizeof *j); j->kind = 6; j->pipe = p; g_gpl_opt_q++; job_push_bg(j); }
static int g_handover_on, g_workers_quit, g_scan_on;
static LONG64 g_scan_jobs, g_scan_pipes;
static struct { int w, h, ncol, cf, df, lv; long long n, scene, first; } g_rtlog[256];      /* SMO_VK_RTLOG=1: render-target sizes (nvn_vk_draw.c, where a pass begins) */
static int g_rtlog_n;
static void job_push(Job *j) {                     /* g_sh_lock held */
    j->next = NULL;
    if (g_jq_tail) g_jq_tail->next = j; else g_jq_head = j;
    g_jq_tail = j; g_jq_queued++;
    WakeConditionVariable(&g_jq_cv);
}
static void job_push_front(Job *j) {               /* g_sh_lock held: needed by the batch about to be replayed */
    j->next = g_jq_head;
    g_jq_head = j;
    if (!g_jq_tail) g_jq_tail = j;
    g_jq_queued++;
    WakeConditionVariable(&g_jq_cv);
}
static void job_copy_src(Job *j, uint8_t **d, int i, const SSrc *s) {
    j->has[i] = 1; j->src[i] = *s;
    memcpy(*d, s->code, (size_t)s->code_len); j->src[i].code = *d; *d += (s->code_len + 7) & ~7;
    if (s->cdata) { memcpy(*d, s->cdata, s->cdata_size); j->src[i].cdata = *d; *d += (s->cdata_size + 7) & ~7u; }
}
/* g_sh_lock held: queues the pipeline of this pair for signature i, unless the pair was already drawn, has had its three
 * predictions, or the pipeline exists. */
static void predict_pair(PairRec *pr, int i) {
    if (pr->drawn || pr->npred >= 3 || (pr->sigs >> i & 1)) return;
    PKey k = g_sig[i].k;
    k.vs = pr->vs->id; k.fs = pr->fs ? pr->fs->id : 0; k.sshade = pr->fs ? pr->sshade : 0;
    uint64_t h = pkey_hash(&k);
    pr->sigs |= 1ull << i;
    if (pipe_find(&k, h)) return;
    pr->npred++;
    Job *j = calloc(1, sizeof *j);
    j->kind = 1; j->pipe = pipe_new(&k, pr->vs, pr->fs, h, 0); j->pipe->predicted = 1;
    g_pred_made++;
    job_push(j);
}
/* Render thread, at a draw: the pipeline of the key; made here, or waited for, when it is not ready (the frame is late). */
/* render thread: the pipelines made and used so far by their key, direct-mapped, in front of g_sh_lock and the byte-wise
 * hash of pipe_get (pipelines are never freed before the end) */
static Pipe *g_pipe_hot[16384];
static unsigned pkey_slot(const PKey *k) {
    const uint8_t *b = (const uint8_t *)k;
    uint64_t h = 0x9E3779B97F4A7C15ull;
    size_t i = 0;
    for (; i + 8 <= sizeof *k; i += 8) { uint64_t w; memcpy(&w, b + i, 8); h = (h ^ w) * 0xff51afd7ed558ccdull; h ^= h >> 32; }
    for (; i < sizeof *k; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    return (unsigned)(h ^ (h >> 29)) & 16383;
}
static Pipe *pipe_get2(const PKey *k, VStage *vs, VStage *fs);
static Pipe *pipe_get(const PKey *k, VStage *vs, VStage *fs) {
    unsigned slot = pkey_slot(k);
    Pipe *c = g_pipe_hot[slot];
    if (c && c->state == 2 && c->used && !memcmp(&c->k, k, sizeof *k)) return c;
    Pipe *p = pipe_get2(k, vs, fs);
    if (p && p->state == 2 && p->used) g_pipe_hot[slot] = p;
    return p;
}
static Pipe *pipe_get2(const PKey *k, VStage *vs, VStage *fs) {
    uint64_t h = pkey_hash(k);
    int mine = 0, waited = 0;
    LONG64 t0 = 0;
    EnterCriticalSection(&g_sh_lock);
    Pipe *p = pipe_find(k, h);
    int was_queued = 0;
    if (skip_active() && !(p && p->state == 2)) {  /* shader_wait = skip: a pipeline that is not there is made by a worker; ready stage libraries are linked here (microseconds) */
        int libs_ready = D.has_gpl && gpl_sep() && !k->sshade && (fs ? vs->lib_state == 2 && fs->lib_state == 2 : vs->lib2_state == 2);
        if (!(libs_ready && (!p || p->state == 0))) {
            if (!p) { Job *j = calloc(1, sizeof *j); j->kind = 1; j->pipe = pipe_new(k, vs, fs, h, 0); job_push_front(j); }
            LeaveCriticalSection(&g_sh_lock);
            skip_note(1, vs->hb, fs ? fs->hb : 0);
            return NULL;
        }
    }
    if (!p) { p = pipe_new(k, vs, fs, h, 1); mine = 1; }
    else if (p->state == 0) { p->state = 1; mine = 1; was_queued = 1; }          /* queued, no worker has begun: made here rather than waited for behind the queue */
    else if (p->state == 1) { waited = 1; t0 = tk(); while (p->state < 2) WD_SLEEP(&g_sh_cv, &g_sh_lock, INFINITE); }
    if (!p->used) {
        int idx, i = sig_note(k, &idx);
        PairRec *pr0 = pair_get(vs, fs);
        pr0->drawn = 1;
        if (D.has_gpl && !gpl_sep() && fs && fs->fragonly) gpl_shared_vs(vs);
        if (i >= 0 && g_predict) for (PairRec *pr = g_pair_all; pr; pr = pr->all) predict_pair(pr, i);      /* a pass signature never seen: the pairs not yet drawn get it now, on the workers */
    }
    LeaveCriticalSection(&g_sh_lock);
    if (waited) {
        LONG64 dt = tk() - t0; hold_note(H_PIPE_WAIT, dt); g_tm[5] += dt;
        /* a wait for a pipeline a worker was making is a hold too */
        if (holdlog_on()) { LARGE_INTEGER fq; QueryPerformanceFrequency(&fq); printf("nvn_vk_hold: frame %lld (%s): PIPELINE waited for (a worker was making it), %.2f ms: vertex code %016llx (key %x %x %x %x, forced outputs %x), fragment code %016llx, topology class %d, %d colour (format %d), depth %d; made %s\n",
                                 (long long)nvn_front_frame_next(), g_in_scene ? "scene" : "no scene", (double)dt * 1000.0 / (double)fq.QuadPart, (unsigned long long)vs->hb, vs->key[0], vs->key[1], vs->key[2], vs->key[3], vs->fo, (unsigned long long)(fs ? fs->hb : 0),
                                 k->topo_class, k->ncolor, k->cfmt[0], k->dfmt, !D.has_gpl ? "whole (no libraries)" : p->lib_how == 0 ? "by a link of ready stage libraries" : "with a library compiled for it"); }
    }
    if (mine) {
        t0 = tk();
        g_render_tid = GetCurrentThreadId();
        double ms = pipe_build(p);
        LONG64 dt = tk() - t0;
        g_tm[5] += dt;
        if (!(D.has_gpl && p->lib_how == 0)) { hold_note(H_PIPE_DRAW, dt); g_pipe_miss++; }      /* a link of ready libraries is not a hold (counted on its own: "linked at a draw") */
        if (holdlog_on() && !(D.has_gpl && p->lib_how == 0)) printf("nvn_vk_hold: frame %lld (%s): PIPELINE made at the draw, %.2f ms: vertex code %016llx (variant key %x %x %x %x, forced outputs %x), fragment code %016llx, topology class %d, %d colour (format %d), depth %d, samples %d; it was %s\n",
                                 (long long)nvn_front_frame_next(), g_in_scene ? "scene" : "no scene", ms, (unsigned long long)vs->hb, vs->key[0], vs->key[1], vs->key[2], vs->key[3], vs->fo, (unsigned long long)(fs ? fs->hb : 0),
                                 k->topo_class, k->ncolor, k->cfmt[0], k->dfmt, k->samples, was_queued ? "queued for a worker that had not begun" : "not known to anyone (no record, no prediction)");
        if (D.debug) printf("nvn_vk: pipeline made at a draw (%lld): stages %u/%u, class %d, %d colour (format %d), depth %d, samples %d: %.1f ms\n", (long long)g_pipe_miss, k->vs, k->fs, k->topo_class, k->ncolor, k->cfmt[0], k->dfmt, k->samples, ms);
    }
    if (!p->used) {
        p->used = 1;
        if (p->predicted && !mine) g_pred_used++;
        if (!p->recorded) { piperec_append(k, vs, fs, 0); p->recorded = 1; }
    }
    return p;
}
/* a pipeline with a geometry stage (only the lightning effects of the game have one: three geometry stages in all). Render
 * thread: made at its first draw, whole (no libraries), and kept in a list of its own: not in pipes.bin, not predicted, as the
 * workers are never handed a program with a geometry stage (nvn_vk_program_set). The key is the draw's plus the geometry
 * variant, which holds the draw's primitive class. */
typedef struct GsPipe { PKey k; uint32_t gs; Pipe *p; struct GsPipe *next; } GsPipe;
static GsPipe *g_gspipes;
static LONG64 g_gs_draws, g_gs_pipes;
static Pipe *gs_pipe_get(const PKey *k, VStage *vs, VStage *gs, VStage *fs) {
    static int dbg = -1;                           /* SMO_VK_GS_DEBUG=3: these draws in magenta, without the depth test (draw_prepare) */
    if (dbg < 0 && (dbg = gs_dbg_mode() == 3) != 0) {
        uint64_t h;
        g_gs_dbg_fs = module_from_glsl(1, "#version 460\nlayout(location = 0) out vec4 o0;\nvoid main() { o0 = vec4(200.0, 0.0, 200.0, 1.0); }\n", "geometry stage debug", &h, NULL);
    }
    for (GsPipe *q = g_gspipes; q; q = q->next) if (q->gs == gs->id && !memcmp(&q->k, k, sizeof *k)) return q->p;
    GsPipe *q = calloc(1, sizeof *q);
    Pipe *p = calloc(1, sizeof *p);
    q->k = *k; q->gs = gs->id; q->p = p;
    p->k = *k; p->vs = vs; p->fs = fs; p->gs = gs; p->state = 1; p->used = 1;
    LONG64 t0 = tk();
    g_render_tid = GetCurrentThreadId();
    double ms = pipe_build(p);
    hold_note(H_PIPE_DRAW, tk() - t0); g_pipe_miss++; g_gs_pipes++;
    printf("nvn_vk: pipeline with a geometry stage made at a draw: stages %u/%u/%u (geometry code %016llx, %d bytes, primitive class %d), %d colour (format %d), depth %d, samples %d: %.1f ms\n",
           k->vs, gs->id, k->fs, (unsigned long long)gs->hb, gs->code_len, gs->gprim, k->ncolor, k->cfmt[0], k->dfmt, k->samples, ms);
    q->next = g_gspipes; g_gspipes = q;
    return p;
}
/* Any thread: the compute pipeline of a stage. Returns 1 when this call made it, 2 when it waited for another thread, 0 when it was there. */
static int compute_pipe_ensure(VStage *v) {
    int how = 0;
    if (v->cp_state == 2) return 0;
    EnterCriticalSection(&g_sh_lock);
    if (v->cp_state == 0) { v->cp_state = 1; how = 1; }
    else if (v->cp_state == 1) { how = 2; while (v->cp_state < 2) WD_SLEEP(&g_sh_cv, &g_sh_lock, INFINITE); }
    LeaveCriticalSection(&g_sh_lock);
    if (how == 1) {
        compute_pipe_make(v);
        EnterCriticalSection(&g_sh_lock);
        v->cp_state = 2;
        WakeAllConditionVariable(&g_sh_cv);
        LeaveCriticalSection(&g_sh_lock);
    }
    return how;
}

/* ---------------------------------------------------------------- the workers */
static HANDLE g_pw_thread[16], g_pw_gate;
/* the render thread is about to wait for something a worker is making, and that worker may run below normal priority
 * (pw_main): every worker back to normal priority (each sets its own again at its next job). */
static int g_pw_nthread;
static void workers_boost(void) {
    if (GetCurrentThreadId() != g_render_tid) return;
    for (int i = 0; i < g_pw_nthread; i++) if (g_pw_thread[i]) SetThreadPriority(g_pw_thread[i], THREAD_PRIORITY_NORMAL);
    g_boosts++;
}
static int g_pw_nthread, g_pw_n, g_pw_waited;
typedef struct { uint32_t magic, compute; uint64_t vs_h, fs_h; PKey k; } PipeRecIn;
static PipeRec *g_pw_rec;
static volatile LONG g_pw_next, g_pw_left;
static LONG64 g_pw_t0, g_pw_t1;
static char g_pcache_path[160];
static VStage *pw_stage(int s, uint64_t vh) {      /* g_sh_lock held: a variant named by pipes.bin, from its record */
    StageRec r;
    VkShaderModule mod;
    for (VStage *v = g_vh[vh & 4095]; v; v = v->next_h) if (v->h == vh && v->stage == s) return v;
    if (!stage_rec_read(s, vh, &r) || !(mod = module_from_spv(r.spv))) return NULL;
    VStage *v = vstage_new(r.hb, s, r.code_len, r.key, r.fo);
    vstage_from_rec(v, &r, vh, mod);
    v->next_h = g_vh[vh & 4095]; g_vh[vh & 4095] = v; v->state = 2;
    InterlockedIncrement64(&R.modules); InterlockedIncrement64(&g_pw_stages);
    return v;
}
/* A worker that is about to make a vertex stage gives the fragment stage of the same program to another worker first
 * (kind 3), so that a program created in the middle of a scene costs the longer of the two stages, not their sum. */
static void spawn_stage(const SSrc *src, const uint32_t *key) {
    Job *j = calloc(1, sizeof *j + (size_t)src->code_len + src->cdata_size + 16);
    uint8_t *d = j->data;
    j->kind = 3; memcpy(j->key1, key, 16);
    job_copy_src(j, &d, 1, src);
    EnterCriticalSection(&g_sh_lock);
    job_push_front(j);
    LeaveCriticalSection(&g_sh_lock);
}
/* ---- pairs nobody announces. A program without a vertex stage is drawn with the
 * vertex stage of ANOTHER program, so its pair is only known at the draw. The vertex stages used that way are few (one
 * screen-quad stage made 13 of the 26 unknown pipelines of): every vertex stage ever drawn with such a fragment
 * stage is remembered ("shared"), and the pair library of shared vertex stage x fragment stage without a program of its own
 * is made by the workers (background queue) as soon as both are known. */
static VStage *g_fo[8192], *g_shvs[64];
static int g_nfo, g_nshvs;
static LONG64 g_gpl_cross;
/* SMO_VK_GPL_CROSS: 0 no such pairs are made ahead; 1 (default) only where the fragment stage reads EXACTLY what the vertex
 * stage writes; 2 wherever the draw would take this vertex variant (measured by ssbu-recomp in Super Smash Bros. Ultimate: 3,906 such
 * fragment stages x 21 shared vertex stages = 37,059 libraries asked for, 17,693 compiled in 1,216 s of worker time: the
 * workers were busy through the whole run and it was WORSE than without libraries: 82 holds, 5.8 s). */
static int gpl_cross_mode(void) { static int m = -1; if (m < 0) m = getenv("SMO_VK_GPL_CROSS") ? atoi(getenv("SMO_VK_GPL_CROSS")) : 1; return m; }
static int gpl_pair_fits(const VStage *vs, const VStage *fs) {
    if (gpl_cross_mode() == 1) return !vs->fo && fs->in_attr == vs->out_attr;
    return (fs->in_attr & ~(vs->out_attr & ~vs->fo)) == vs->fo;      /* the draw takes the vertex variant whose forced outputs are exactly the fragment inputs the code does not write */
}
static void gpl_cross_push(VStage *vs, VStage *fs) {       /* g_sh_lock held */
    if (!gpl_cross_mode() || !gpl_pair_fits(vs, fs)) return;
    Job *j = calloc(1, sizeof *j);
    j->kind = 5; j->lvs = vs; j->lfs = fs;
    g_gpl_cross++;
    job_push_bg(j);
}
static void gpl_fo_made(VStage *v) {               /* a worker: the fragment stage of a program without a vertex stage is ready */
    if (!D.has_gpl) return;
    EnterCriticalSection(&g_sh_lock);
    if (!v->fragonly && g_nfo < 8192) { v->fragonly = 1; g_fo[g_nfo++] = v; for (int i = 0; i < g_nshvs; i++) gpl_cross_push(g_shvs[i], v); }
    LeaveCriticalSection(&g_sh_lock);
}
static void gpl_shared_vs(VStage *vs) {            /* g_sh_lock held, render thread: vs was drawn with a fragment stage of a program without a vertex stage */
    for (int i = 0; i < g_nshvs; i++) if (g_shvs[i] == vs) return;
    if (g_nshvs == 64) return;
    g_shvs[g_nshvs++] = vs;
    for (int i = 0; i < g_nfo; i++) gpl_cross_push(vs, g_fo[i]);
}
static void spawn_stage_fo(const SSrc *src, const uint32_t *key) {      /* as spawn_stage, marked: the stage of a program without a vertex stage */
    Job *j = calloc(1, sizeof *j + (size_t)src->code_len + src->cdata_size + 16);
    uint8_t *d = j->data;
    j->kind = 3; j->fragonly = 1; memcpy(j->key1, key, 16);
    job_copy_src(j, &d, 1, src);
    EnterCriticalSection(&g_sh_lock);
    job_push_front(j);
    LeaveCriticalSection(&g_sh_lock);
}
/* The barrier: a frame that is a loading screen (at most 60 draws) is not presented while the workers still have
 * a queued stage, pair library or pipeline: the game leaves its loading screen only when everything handed over is ready.
 * On by default with pipeline libraries (SMO_VK_BARRIER=0 / =1 forces it). Time-out 20 s, logged loudly. */
static LONG64 g_bar_n, g_bar_tk, g_bar_max, g_bar_timeout;
static void wd_dump(double stall_s, LONG64 tries_since);     /* nvn_vk_watchdog.c */
static void loading_barrier(LONG64 frame, LONG64 draws) {
    static int on = -1;
    int want_dump = 0;
    if (on < 0) { const char *e = getenv("SMO_VK_BARRIER"); on = e ? atoi(e) != 0 : D.has_gpl; }
    if (!on || draws > 60 || !g_pw_nthread) return;
    EnterCriticalSection(&g_sh_lock);
    if (g_jq_queued != g_jq_done || g_bq_queued != g_bq_done) {
        LONG64 t0 = tk();
        LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
        LONG64 d0 = g_jq_done + g_bq_done, tlast = t0;
        while ((g_jq_queued != g_jq_done || g_bq_queued != g_bq_done) && tk() - t0 < 20 * fq.QuadPart) {
            WD_SLEEP(&g_sh_cv, &g_sh_lock, 20);
            if (tk() - t0 > fq.QuadPart * 15 / 100 && tk() - tlast > fq.QuadPart / 10) {      /* held longer than 150 ms: the same picture again, with the progress bar, ten times a second */
                LONG64 tot = g_jq_queued + g_bq_queued - d0, dn = g_jq_done + g_bq_done - d0;
                tlast = tk();
                LeaveCriticalSection(&g_sh_lock);
                present_progress(tot > 0 ? (float)dn / (float)tot : 0.0f);
                EnterCriticalSection(&g_sh_lock);
            }
        }
        LONG64 dt = tk() - t0;
        g_bar_n++; g_bar_tk += dt; if (dt > g_bar_max) g_bar_max = dt;
        if (g_jq_queued != g_jq_done || g_bq_queued != g_bq_done) {
            static int nrep;
            if (nrep == 0 && getenv("SMO_VK_BARRIER_DUMP")) want_dump = 1;      /* debug aid: every thread's call stack, once (the watchdog's report) */
            if (nrep++ < 2) for (int w = 0; w < g_wk_n && w < 64; w++) if (g_wk[w].kind >= 0)
                printf("nvn_vk_barrier:   worker %d (thread %lu) busy for %.1f s with job kind %ld (queue %ld) of program #%ld\n", w, (unsigned long)g_wk[w].tid,
                       (double)(tk() - g_wk[w].t0) / (double)fq.QuadPart, (long)(g_wk[w].kind / 10), (long)(g_wk[w].kind % 10), (long)g_wk[w].serial);
        }
        if (g_jq_queued != g_jq_done || g_bq_queued != g_bq_done) { g_bar_timeout++; printf("nvn_vk_barrier: TIME-OUT at frame %lld: after 20 s the workers still have %lld + %lld jobs: the frame is presented, pipelines may be made at a draw\n", (long long)frame, (long long)(g_jq_queued - g_jq_done), (long long)(g_bq_queued - g_bq_done)); }
        else if (dt * 10 > fq.QuadPart) printf("nvn_vk_barrier: frame %lld (a loading screen: %lld draws) was held %.0f ms until the workers had finished every queued stage and pipeline\n", (long long)frame, (long long)draws, (double)dt * 1000.0 / (double)fq.QuadPart);
    }
    LeaveCriticalSection(&g_sh_lock);
    if (want_dump) wd_dump(20.0, 0);
}
/* The variants of one code that only a draw decides (integer vertex attributes, integer textures, vertex outputs
 * forced by the fragment stage of the draw) are listed in the code's .var file by the run that first needed them. Here,
 * when the program is handed over (loading), each listed variant is made from its record, so the draw finds it. */
static void variants_known(const SSrc *src, uint64_t hb) {
    char path[300];
    VarRec e[32];
    int n, how;
    snprintf(path, sizeof path, "%s/stage/%d_%016llx.var", vkc_dir(), src->stage, (unsigned long long)hb);
    FILE *fp = fopen(path, "rb");
    if (!fp) return;
    n = (int)fread(e, sizeof e[0], 32, fp);
    fclose(fp);
    for (int i = 0; i < n; i++) {
        if (e[i].magic != STAGEREC_MAGIC || !(e[i].key[0] | e[i].key[1] | e[i].key[2] | e[i].key[3] | e[i].fo)) continue;
        stage_ensure(src, hb, e[i].key, e[i].fo, &how);
        if (how == 1) { InterlockedIncrement64(&g_w_stages); InterlockedIncrement64(&g_var_known); }
    }
}
static void job_prog(Job *j) {
    static const uint32_t zero[4];
    int how;
    InterlockedIncrement64(&g_w_progs);
    if (j->has[0] && j->has[1]) spawn_stage(&j->src[1], zero);
    if (j->has[2]) {
        VStage *c = stage_ensure(&j->src[2], ssrc_hb(&j->src[2]), zero, 0, &how);
        if (how == 1) InterlockedIncrement64(&g_w_stages);
        if (c->ok && !c->images && c->nsmp <= 32) compute_pipe_ensure(c);
    }
    if (!j->has[0]) return;
    uint64_t hv = ssrc_hb(&j->src[0]);
    VStage *vs = stage_ensure(&j->src[0], hv, zero, 0, &how), *fs = NULL;
    if (how == 1) InterlockedIncrement64(&g_w_stages);
    if (j->has[1]) { fs = stage_ensure(&j->src[1], ssrc_hb(&j->src[1]), zero, 0, &how); if (how == 1) InterlockedIncrement64(&g_w_stages); }
    if (!vs->ok || (fs && !fs->ok)) return;
    variants_known(&j->src[0], hv);                /* what earlier runs needed of this code at a draw */
    if (fs) variants_known(&j->src[1], fs->hb);
    if (fs && (fs->in_attr & ~vs->out_attr)) {    /* fragment inputs the vertex code never writes: the variant that declares them */
        vs = stage_ensure(&j->src[0], hv, zero, fs->in_attr & ~vs->out_attr, &how);
        if (how == 1) InterlockedIncrement64(&g_w_stages);
        if (!vs->ok) return;
    }
    if (D.has_gpl && gpl_sep() && !(fs && j->sshade) && stage_fits_universal(vs) && (!fs || stage_fits_universal(fs))) { int lh; stagelib_ensure(vs, 0, &lh); if (fs) stagelib_ensure(fs, 0, &lh); else stagelib_ensure(vs, 1, &lh); }      /* SMO_VK_GPL=2: a library per stage (a program without a fragment stage: both forms of its vertex stage) */
    else
    if (D.has_gpl) { int lh; pairlib_ensure(vs, fs, 1, fs ? j->sshade : 0, &lh); }      /* the pair's shaders compiled now, without pass formats; a draw links */
    if (!g_predict) return;
    EnterCriticalSection(&g_sh_lock);
    PairRec *pr = pair_get(vs, fs);
    pr->sshade = (uint8_t)j->sshade;
    for (int n = 0; n < 3 && !pr->drawn; n++) {    /* the three signatures most used so far */
        int best = -1;
        for (int i = 0; i < g_nsig; i++) if (!(pr->sigs >> i & 1) && (best < 0 || g_sig[i].uses > g_sig[best].uses)) best = i;
        if (best < 0) break;
        predict_pair(pr, best);
    }
    LeaveCriticalSection(&g_sh_lock);
}
static void job_draw(Job *j) {                     /* a draw the look-ahead met whose stages were not all made yet */
    int how;
    VStage *fs = NULL;
    if (j->has[1]) spawn_stage(&j->src[1], j->key1);
    uint64_t hv = ssrc_hb(&j->src[0]);
    VStage *vs = stage_ensure(&j->src[0], hv, j->key0, 0, &how);
    if (how == 1) InterlockedIncrement64(&g_w_stages);
    if (j->has[1]) { fs = stage_ensure(&j->src[1], ssrc_hb(&j->src[1]), j->key1, 0, &how); if (how == 1) InterlockedIncrement64(&g_w_stages); if (!fs->ok) return; }
    if (!vs->ok) return;
    if (fs && (fs->in_attr & ~vs->out_attr)) { vs = stage_ensure(&j->src[0], hv, j->key0, fs->in_attr & ~vs->out_attr, &how); if (how == 1) InterlockedIncrement64(&g_w_stages); if (!vs->ok) return; }
    if (!D.has_eds3) return;                       /* the key holds blend state the look-ahead does not follow */
    PKey k = j->k;
    k.vs = vs->id; k.fs = fs ? fs->id : 0;
    uint64_t h = pkey_hash(&k);
    int mine = 0;
    EnterCriticalSection(&g_sh_lock);
    Pipe *p = pipe_find(&k, h);
    if (!p) { p = pipe_new(&k, vs, fs, h, 1); mine = 1; } else if (p->state == 0) { p->state = 1; mine = 1; }
    LeaveCriticalSection(&g_sh_lock);
    if (mine) { pipe_build(p); InterlockedIncrement64(&g_w_pipes); }
}
static DWORD WINAPI pw_main(void *arg) {
    (void)arg;
    os_pin_host_thread();
    LONG wslot = InterlockedIncrement(&g_wk_n) - 1;
    if (wslot < 64) { g_wk[wslot].kind = -1; g_wk[wslot].tid = GetCurrentThreadId(); }
    for (;;) {                                     /* 1. the keys of earlier runs */
        LONG i = InterlockedIncrement(&g_pw_next) - 1;
        if (i >= g_pw_n) break;
        PipeRec *r = &g_pw_rec[i];
        EnterCriticalSection(&g_sh_lock);
        VStage *vs = pw_stage(r->compute ? 5 : 0, r->vs_h), *fs = r->fs_h ? pw_stage(1, r->fs_h) : NULL;
        Pipe *p = NULL;
        if (vs && !r->compute && (!r->fs_h || fs)) {
            r->k.vs = vs->id; r->k.fs = fs ? fs->id : 0;
            uint64_t h = pkey_hash(&r->k);
            if (!pipe_find(&r->k, h)) { p = pipe_new(&r->k, vs, fs, h, 1); p->recorded = 1; }
            int idx;
            sig_note(&r->k, &idx);
            pair_get(vs, fs)->drawn = 1;           /* a pair with recorded keys needs no prediction */
        }
        LeaveCriticalSection(&g_sh_lock);
        if (!vs || (r->fs_h && !fs)) { InterlockedIncrement64(&g_pw_failed); continue; }      /* a stage record or SPIR-V file is gone: made when the game hands the program over, as on a first run */
        if (r->compute) { vs->cp_recorded = 1; if (compute_pipe_ensure(vs) == 1) InterlockedIncrement64(&g_pw_pipes); continue; }
        if (p) { pipe_build(p); InterlockedIncrement64(&g_pw_pipes); }
    }
    InterlockedExchange64(&g_pw_t1, tk());
    if (InterlockedDecrement(&g_pw_left) == 0) SetEvent(g_pw_gate);
    int prio = THREAD_PRIORITY_NORMAL;
    for (;;) {                                     /* 2. what the game hands over while it runs */
        {   /* while a scene is drawn the workers run BELOW the game's threads (measured by ssbu-recomp in Super Smash Bros. Ultimate: 16
             * workers at normal priority kept the CPU 80% busy and the main thread's frames took 32-40 ms).
             * SMO_VK_WORKER_PRIO=0 keeps them at normal priority. */
            static int on = -1;
            if (on < 0) on = !(getenv("SMO_VK_WORKER_PRIO") && !atoi(getenv("SMO_VK_WORKER_PRIO")));
            int want = on && g_in_scene ? THREAD_PRIORITY_BELOW_NORMAL : THREAD_PRIORITY_NORMAL;
            if (want != prio) { SetThreadPriority(GetCurrentThread(), want); prio = want; }
        }
        EnterCriticalSection(&g_sh_lock);
        while (!g_jq_head && !g_bq_head && !g_workers_quit) SleepConditionVariableCS(&g_jq_cv, &g_sh_lock, INFINITE);
        Job *j = g_jq_head;
        int bg = 0;
        if (j) { g_jq_head = j->next; if (!g_jq_head) g_jq_tail = NULL; }
        else if (!g_workers_quit && (j = g_bq_head) != NULL) { g_bq_head = j->next; if (!g_bq_head) g_bq_tail = NULL; bg = 1; }      /* at exit the background work is left undone */
        Pipe *p = NULL;
        if (j && j->kind == 1 && j->pipe->state == 0) { p = j->pipe; p->state = 1; }
        LeaveCriticalSection(&g_sh_lock);
        if (!j) break;
        if (wslot < 64) { g_wk[wslot].serial = j->has[1] ? j->src[1].serial : j->has[0] ? j->src[0].serial : 0; g_wk[wslot].t0 = tk(); g_wk[wslot].kind = j->kind * 10 + bg; }
        if (j->kind == 0) job_prog(j);
        else if (j->kind == 2) job_draw(j);
        else if (j->kind == 3) { int how; VStage *v = stage_ensure(&j->src[1], ssrc_hb(&j->src[1]), j->key1, 0, &how); if (how == 1) InterlockedIncrement64(&g_w_stages); if (v && v->ok && D.has_gpl && gpl_sep() && stage_fits_universal(v)) { int lh; stagelib_ensure(v, 0, &lh); } else if (j->fragonly && v && v->ok) gpl_fo_made(v); }
        else if (j->kind == 5) { int lh; pairlib_ensure(j->lvs, j->lfs, 1, 0, &lh); }
        else if (j->kind == 6) gpl_optimise(j->pipe);
        else if (j->kind == 7) { int how, lh; VStage *v = stage_ensure(&j->src[1], ssrc_hb(&j->src[1]), j->key1, (uint32_t)j->sshade, &how); if (how == 1) InterlockedIncrement64(&g_w_stages); if (v && v->ok && D.has_gpl && gpl_sep() && stage_fits_universal(v)) stagelib_ensure(v, 0, &lh); }      /* the stage a skipped draw waits for */
        else if (p) { pipe_build(p); InterlockedIncrement64(&g_w_pipes); }
        if (wslot < 64) g_wk[wslot].kind = -1;
        free(j);
        EnterCriticalSection(&g_sh_lock);
        if (bg) g_bq_done++; else g_jq_done++;
        WakeAllConditionVariable(&g_sh_cv);
        LeaveCriticalSection(&g_sh_lock);
    }
    return 0;
}
/* nvnProgramSetShaders, GUEST thread (called by the front end when the renderer is Vulkan): the program's stages go to
 * the workers now. The caller is held only while more jobs wait than there are workers (a burst of a loading thread),
 * so that loading ends when its shaders are ready; a single program created during a scene does not hold its caller. */
/* SMO_VK_STAGECOPY=1 (off by default until it has a whole verification). Measured in a first run (hold
 * log): 32 of the 34 stages made or waited for at a draw had a code hash that NOBODY had handed over, although their programs
 * were set long before: Stage.code points into the GAME's memory and that memory no longer held at the draw what it held at
 * nvnProgramSetShaders (a console's driver compiles at that call and never reads the memory again). With the switch the
 * stage's bytes (program header, code, constant-buffer-1 data, in their relative places) are copied at the call and the
 * stage points at the copy; the copy of an earlier call for the same stage is freed. */
static struct { Stage *st; void *blk; } g_scopy[65536];
static LONG64 g_scopy_n, g_scopy_bytes;
static void stage_copy(Stage *st, int s) {
    if (!st->code || st->code_len <= 0) return;
    const uint8_t *lo = st->code - (s == 5 ? 0 : 0x30), *hi = st->code + st->code_len;
    if (st->ctl[3] && st->ctl[4] && lo + st->ctl[4] + st->ctl[3] > hi) hi = lo + st->ctl[4] + st->ctl[3];
    size_t n = (size_t)(hi - lo);
    if (n > (16u << 20)) return;
    unsigned i = (unsigned)(((uintptr_t)st * 0x9E3779B97F4A7C15ull) >> 48);
    int k;
    for (k = 0; k < 64; k++, i = (i + 1) & 65535) if (g_scopy[i].st == st || !g_scopy[i].st) break;
    if (k == 64) return;                           /* table neighbourhood full: the stage keeps pointing at the game's memory */
    if (g_scopy[i].st == st && st->code >= (const uint8_t *)g_scopy[i].blk && st->code < (const uint8_t *)g_scopy[i].blk + (16u << 20) && lo == (const uint8_t *)g_scopy[i].blk) return;      /* already the copy */
    uint8_t *blk = malloc(n);
    if (!blk) return;
    memcpy(blk, lo, n);
    free(g_scopy[i].blk);
    g_scopy[i].st = st; g_scopy[i].blk = blk;
    st->code = blk + (st->code - lo);
    g_scopy_n++; g_scopy_bytes += (LONG64)n;
}
/* programs handed over per game thread (the exit report): which threads load shaders, how many near a scene, how long
 * the hand-over held them */
#define HOTHR_N 32
static struct { const char *name; LONG64 n, near_scene, held, held_tk; } g_hothr[HOTHR_N];
static volatile LONG g_hothr_n;
static __declspec(thread) int t_hothr = -1;
static int hothr_slot(void) {
    if (t_hothr >= 0) return t_hothr;
    LONG i = InterlockedIncrement(&g_hothr_n) - 1;
    if (i >= HOTHR_N) return t_hothr = HOTHR_N - 1;            /* more threads than slots: the last slot takes the rest */
    const char *guest_thread_name_of(DWORD tid);                   /* os_threads.c */
    g_hothr[i].name = guest_thread_name_of(GetCurrentThreadId());
    return t_hothr = (int)i;
}
void nvn_vk_program_set(void *prog) {
    Prog *p = prog;
    SSrc s[3];
    {   static volatile LONG first;                /* the process before any stage, library or pipeline of the game exists (nvn_vk_mem lines follow every 300 frames) */
        if (!first && InterlockedExchange(&first, 1) == 0) {
            PROCESS_MEMORY_COUNTERS_EX pm;
            memset(&pm, 0, sizeof pm); pm.cb = sizeof pm;
            K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pm, sizeof pm);
            printf("nvn_vk_mem: the game hands its first program over (frame %lld): working set %llu MB, private bytes %llu MB\n", (long long)nvn_front_frame_next(), (unsigned long long)(pm.WorkingSetSize >> 20), (unsigned long long)(pm.PrivateUsage >> 20));
        }
    }
    if (p->st[2].state) { static int nl; if (nl++ < 20) printf("nvn_vk: program #%d has a geometry stage (%d bytes): its stages and pipelines are made at its first draw\n", p->serial, p->st[2].code_len); }
    if (!g_handover_on || p->st[2].state || p->st[3].state || p->st[4].state) return;
    {   static int on = -1;
        if (on < 0) on = getenv("SMO_VK_STAGECOPY") && atoi(getenv("SMO_VK_STAGECOPY"));
        if (on) { EnterCriticalSection(&g_sh_lock); stage_copy(&p->st[0], 0); stage_copy(&p->st[1], 1); stage_copy(&p->st[5], 5); LeaveCriticalSection(&g_sh_lock); }
    }
    int has[3] = {ssrc_of(&s[0], p, 0), ssrc_of(&s[1], p, 1), ssrc_of(&s[2], p, 5)};
    {   /* debug aid: SMO_VK_DUMP_SERIAL=<n> also when program n is handed over (its stages may come from the cache) */
        static int want = -2; if (want == -2) want = getenv("SMO_VK_DUMP_SERIAL") ? atoi(getenv("SMO_VK_DUMP_SERIAL")) : -1;
        if (want >= 0 && p->serial == want)
            for (int i = 0; i < 3; i++) if (has[i]) {
                const char *dir = getenv("SMO_SHADERDUMP"); char path[300];
                snprintf(path, sizeof path, "%s%svk_prog_%05d_%d.bin", dir ? dir : "", dir ? "/" : "", p->serial, i == 2 ? 5 : i);
                FILE *f = fopen(path, "wb"); if (f) { fwrite(s[i].code, 1, (size_t)s[i].code_len, f); fclose(f); }
            }
    }
    if (!has[0] && !has[2] && has[1]) {
        /* SMO_VK_FRAGONLY. A program
         * with a fragment stage and no vertex stage (the game binds stages of different programs) was never handed over:
         * its fragment stage was translated at its first draw. Believed to be the "code never handed over" stages of the
         * hold log (28-32 of the 29-34 stage holds of a first run: all fragment stages of programs set at start). */
        /* ON by default (SMO_VK_FRAGONLY=0 turns it off). Measured on a first run with an empty cache, same exe, quiet PC:
         * off 24 stages made at a draw (297 ms) + 13 waited for (127 ms), 123 holds / 730 ms in all, 0 of 6 windows
         * of play clean; on 0 made + 4 waited for (29 ms), 81 holds / 250 ms, 6 of 8 clean. The price is at loading:
         * 7,740 stages made by the workers instead of 3,872, callers of nvnProgramSetShaders held 17.5 s instead of 7.4 s
         * over the whole first run (a second run reads them from the cache). */
        static int on = -1;
        if (on < 0) on = !(getenv("SMO_VK_FRAGONLY") && !atoi(getenv("SMO_VK_FRAGONLY")));
        if (on) { static const uint32_t zero[4]; if (holdlog_on()) { EnterCriticalSection(&g_sh_lock); ho_note(ssrc_hb(&s[1])); LeaveCriticalSection(&g_sh_lock); } spawn_stage_fo(&s[1], zero); }
        return;
    }
    if (!has[0] && !has[2]) return;
    LONG64 ta = tk();
    size_t need = 0;
    for (int i = 0; i < 3; i++) if (has[i]) need += (size_t)s[i].code_len + s[i].cdata_size + 16;
    Job *j = calloc(1, sizeof *j + need);
    uint8_t *d = j->data;
    j->sshade = p->sample_shading;
    for (int i = 0; i < 3; i++) if (has[i]) job_copy_src(j, &d, i, &s[i]);
    uint64_t hn[2] = {0, 0};
    if (holdlog_on()) for (int i = 0; i < 2; i++) if (has[i]) hn[i] = ssrc_hb(&s[i]);      /* hashed before the lock is taken */
    LONG64 tb = tk();
    nvn_setsh_part[5] += tb - ta;
    EnterCriticalSection(&g_sh_lock);
    nvn_setsh_part[6] += tk() - tb;
    if (holdlog_on()) for (int i = 0; i < 2; i++) if (has[i]) ho_note(hn[i]);
    job_push(j);
    /* away from a scene (no compute dispatch in the last 600 frames: loading) the caller is also held while the background queue
     * still has work: loading ends when everything it holds is ready */
    int fr = scene_recent(), ht = hothr_slot();
    g_hothr[ht].n++; g_hothr[ht].near_scene += fr;
    /* near a scene the caller (a game thread) is not held at all: a draw that needs the program waits for it
     * on the render thread, a program not drawn in this frame costs nothing */
    if (!fr && (g_jq_queued - g_jq_done > g_pw_nthread || g_bq_queued > g_bq_done)) {
        LONG64 t0 = tk();
        LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
        while ((g_jq_queued - g_jq_done > g_pw_nthread || g_bq_queued > g_bq_done) && tk() - t0 < 30 * fq.QuadPart) WD_SLEEP(&g_sh_cv, &g_sh_lock, 100);
        g_set_held++; g_set_held_tk += tk() - t0;
        g_hothr[ht].held++; g_hothr[ht].held_tk += tk() - t0;
        nvn_setsh_part[7] += tk() - t0;
    }
    LeaveCriticalSection(&g_sh_lock);
}
static int piperec_cmp(const void *a, const void *b) { return memcmp(a, b, sizeof(PipeRec)); }
static void prewarm_start(void) {
    InitializeCriticalSection(&g_sh_lock); InitializeCriticalSection(&g_gpl_lock);
    InitializeConditionVariable(&g_sh_cv); InitializeConditionVariable(&g_jq_cv);
    CreateDirectoryA(vkc_dir(), NULL); CreateDirectoryA(vkc_file("spv"), NULL); CreateDirectoryA(vkc_file("stage"), NULL);
    if (getenv("SMO_VK_CACHE")) printf("nvn_vk: cache folder %s (SMO_VK_CACHE)\n", vkc_dir());
    {   HMODULE m = LoadLibraryA("smo_glslang.dll");
        g_glsl_to_spirv = m ? (void *)GetProcAddress(m, "smo_glsl_to_spirv") : NULL; g_spirv_free = m ? (void *)GetProcAddress(m, "smo_spirv_free") : NULL;
        if (!g_glsl_to_spirv || !g_spirv_free) vkd_fail("smo_glslang.dll cannot be loaded (the link step writes it beside the exe)");
    }
    /* The pipeline cache object costs its entries in the driver's memory: about 640 MB of a game's working set in the Sand
     * Kingdom, the same whether read from the file or filled while the game hands its programs over, and destroying
     * it after the boot frees nothing (the driver keeps the memory). It stays: the NVIDIA driver's own shader disk cache
     * would serve the same pipelines, but it keeps one cache per executable, so an exe started under a new name (every
     * test copy's first run) would compile everything again (several minutes of every processor). SMO_VK_PCACHE=0 makes
     * pipelines without the object (nothing read or saved): 640 MB less for an exe whose name the driver has seen. */
    if (getenv("SMO_VK_PCACHE") && !atoi(getenv("SMO_VK_PCACHE"))) printf("nvn_vk: SMO_VK_PCACHE=0: no pipeline cache object: pipelines are made without one (the driver's own disk cache, kept per executable, serves them); nothing is read or saved\n");
    else {   /* the driver's pipeline cache, ours on disk: keyed by GPU and driver version (the driver checks its own header too) */
        VkPipelineCacheCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        void *data = NULL;
        snprintf(g_pcache_path, sizeof g_pcache_path, "%s/pipelines_%04x_%04x_%08x.bin", vkc_dir(), (unsigned)V.props.vendorID, (unsigned)V.props.deviceID, (unsigned)V.props.driverVersion);
        LONG64 tp0 = tk(), tp1;
        LARGE_INTEGER pfq; QueryPerformanceFrequency(&pfq);
        FILE *fp = fopen(g_pcache_path, "rb");
        if (fp) { _fseeki64(fp, 0, SEEK_END); long long n = _ftelli64(fp); _fseeki64(fp, 0, SEEK_SET); if (n > 0 && (data = malloc((size_t)n)) && fread(data, 1, (size_t)n, fp) == (size_t)n) { ci.initialDataSize = (size_t)n; ci.pInitialData = data; } fclose(fp); }
        tp1 = tk();
        PROCESS_MEMORY_COUNTERS_EX pm0, pm1;
        memset(&pm0, 0, sizeof pm0); pm0.cb = sizeof pm0; pm1 = pm0;
        K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pm0, sizeof pm0);
        if (vkCreatePipelineCache(V.device, &ci, NULL, &g_pcache) != VK_SUCCESS) { ci.initialDataSize = 0; ci.pInitialData = NULL; VKC(vkCreatePipelineCache(V.device, &ci, NULL, &g_pcache)); }
        K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pm1, sizeof pm1);
        printf("nvn_vk_mem: the driver took the pipeline cache in with %+lld MB private bytes, %+lld MB working set\n", ((long long)pm1.PrivateUsage - (long long)pm0.PrivateUsage) >> 20, ((long long)pm1.WorkingSetSize - (long long)pm0.WorkingSetSize) >> 20);
        /* what the whole-file read costs at every start (file read, then the driver taking the data) */
        printf("nvn_vk: pipeline cache %s: %llu KB read (file read %.0f ms, driver took it in %.0f ms: start-up time on the thread that creates the device)\n", g_pcache_path, (unsigned long long)(ci.initialDataSize >> 10), (double)(tp1 - tp0) * 1000.0 / (double)pfq.QuadPart, (double)(tk() - tp1) * 1000.0 / (double)pfq.QuadPart);
        free(data);
    }
    /* Blind prediction (pair x most used pass signatures) is OFF unless SMO_VK_PREDICT=1: measured on a first run it made
     * 8,418 pipelines of which draws used 324, and kept the workers from the programs being handed over. The look-ahead
     * (scan_list below) names exactly what a batch needs instead. */
    g_predict = D.has_eds3 && getenv("SMO_VK_PREDICT") && atoi(getenv("SMO_VK_PREDICT"));
    g_scan_on = D.has_eds3 && !(getenv("SMO_VK_LOOKAHEAD") && !atoi(getenv("SMO_VK_LOOKAHEAD")));
    int ahead = !(getenv("SMO_VK_PREWARM") && !atoi(getenv("SMO_VK_PREWARM")));
    FILE *fp = ahead ? fopen(vkc_file("pipes.bin"), "rb") : NULL;
    if (fp) {
        fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
        g_pw_rec = malloc((size_t)n + 1);
        g_pw_n = (int)(fread(g_pw_rec, sizeof(PipeRec), (size_t)n / sizeof(PipeRec), fp));
        fclose(fp);
        int kept = 0;
        qsort(g_pw_rec, (size_t)g_pw_n, sizeof(PipeRec), piperec_cmp);
        for (int i = 0; i < g_pw_n; i++) if (g_pw_rec[i].magic == PIPEREC_MAGIC && (!kept || memcmp(&g_pw_rec[kept - 1], &g_pw_rec[i], sizeof(PipeRec)))) g_pw_rec[kept++] = g_pw_rec[i];
        g_pw_n = kept;
    } else printf(ahead ? "nvn_vk: no ../cache/vk/pipes.bin: a first run on this PC; stages and pipelines are made when the game hands its programs over\n" : "nvn_vk: SMO_VK_PREWARM=0: the pipeline list of earlier runs is not read\n");
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    g_pw_nthread = (int)si.dwNumberOfProcessors - 2;
    if (g_pw_nthread < 1) g_pw_nthread = 1;
    if (g_pw_nthread > 16) g_pw_nthread = 16;
    if (getenv("SMO_VK_WORKERS") && atoi(getenv("SMO_VK_WORKERS")) > 0 && atoi(getenv("SMO_VK_WORKERS")) <= 16) g_pw_nthread = atoi(getenv("SMO_VK_WORKERS"));
    g_pw_gate = CreateEventA(NULL, TRUE, FALSE, NULL);
    g_pw_left = g_pw_nthread;
    g_pw_t0 = tk();
    for (int i = 0; i < g_pw_nthread; i++) g_pw_thread[i] = CreateThread(NULL, 0, pw_main, NULL, 0, NULL);
    g_handover_on = !(getenv("SMO_VK_HANDOVER") && !atoi(getenv("SMO_VK_HANDOVER")));
    printf("nvn_vk: %d pipelines listed in ../cache/vk/pipes.bin; %d worker threads; programs are handed to them when the game sets their shaders: %s; key prediction: %s\n", g_pw_n, g_pw_nthread,
           g_handover_on ? "yes" : "no (SMO_VK_HANDOVER=0)", g_predict ? "blind (SMO_VK_PREDICT=1)" : g_scan_on ? "look-ahead of every batch" : D.has_eds3 ? "none (SMO_VK_LOOKAHEAD=0)" : "none (blend state is part of the key on this GPU)");
}
/* ---------------------------------------------------------------- look-ahead: what a batch will draw, before it is replayed
 * The lists of a submitted batch are read once for the (stage pair, pass formats, topology class) of every draw. A
 * combination not met before whose stages or pipeline do not exist goes to the FRONT of the worker queue; the replay that
 * follows then finds them made, waits for the one being made, or makes a queued one itself: the work of a frame that needs
 * many new pipelines at once (the first frame of a new scene) is spread over all cores instead of done one by one at the draws.
 * Not followed: integer texture formats (taken as bound now) and, without extended dynamic state 3, anything. */
typedef struct { Prog *prog[2], *gprog /* a geometry stage is bound (its draws are not followed: gs_pipe_get) */; int ncol, nattr, sig_ok, attr_new; TargetRef col[8], depth; uint32_t vattr[32], ints, sints, sig_tag; uint8_t logic; PKey sig; } Scan;
static uint64_t g_scan_seen[8192];
static void scan_sig(Scan *s) {
    memset(&s->sig, 0, sizeof s->sig);
    s->sig_ok = pkey_targets_of(&s->sig, s->ncol, s->col, &s->depth) && (s->ncol || s->depth.tex);
    s->sig_tag = (uint32_t)fnv(0xcbf29ce484222325ULL, &s->sig, sizeof s->sig);
}
/* the integer texture kinds as bound when the batch begins: the scan may run beside the replay (SMO_VK_MT), which changes R */
static uint32_t g_scan_ti[2], g_scan_tsi[2];
static void scan_begin(Scan *s) {
    g_scan_ti[0] = (uint32_t)R.tex_int[0]; g_scan_ti[1] = (uint32_t)R.tex_int[1]; g_scan_tsi[0] = (uint32_t)R.tex_sint[0]; g_scan_tsi[1] = (uint32_t)R.tex_sint[1];
    s->prog[0] = R.prog[0]; s->prog[1] = R.prog[1]; s->gprog = R.prog[2]; s->ncol = R.ncol; memcpy(s->col, R.col, sizeof s->col); s->depth = R.depth;
    s->nattr = R.nattr; memcpy(s->vattr, R.vattr, sizeof s->vattr); s->attr_new = 1; s->logic = R.logic;
    scan_sig(s);
}
static void scan_draw(Scan *s, unsigned prim) {
    static const uint8_t tclass[7] = {0, 1, 1, 1, 2, 2, 2};
    Prog *p0 = s->prog[0], *p1 = s->prog[1];
    if (!p0 || prim > 6 || !s->sig_ok || !p0->st[0].code || (p1 && !p1->st[1].code) || p0->dead || (p1 && p1->dead) || s->gprog) return;
    if (s->attr_new) {                             /* which attributes are integers (as draw_prepare) */
        s->ints = s->sints = 0; s->attr_new = 0;
        for (int i = 0; i < 16 && i < s->nattr; i++) {
            int nf = (int)(s->vattr[i] & 0xff), k = nf >= 0x6D ? nf - 0x6D : (nf - 1) % 12;
            if (((nf >= 1 && nf <= 0x30) || (nf >= 0x6D && nf <= 0x78)) && (k == 2 || k == 3 || k == 7 || k == 8 || k == 10 || k == 11)) { s->ints |= 1u << i; if (k == 3 || k == 8 || k == 11) s->sints |= 1u << i; }
            if (nf == 0x3E) s->ints |= 1u << i;
            if (nf == 0x6A) { s->ints |= 1u << i; s->sints |= 1u << i; }
        }
    }
    uint64_t w[6] = {(uint64_t)(uintptr_t)p0->st[0].code ^ ((uint64_t)p0->serial << 44), p1 ? (uint64_t)(uintptr_t)p1->st[1].code ^ ((uint64_t)p1->serial << 44) : 0, s->sig_tag, tclass[prim] | (uint64_t)s->logic << 8 | (uint64_t)(p1 ? p1->sample_shading & 1 : 0) << 16,
                     (uint64_t)s->ints << 32 | s->sints, (uint64_t)g_scan_ti[0] << 32 | g_scan_ti[1]};
    uint64_t tag = hmix(0xcbf29ce484222325ULL, w, sizeof w) | 1;
    unsigned slot = (unsigned)(tag >> 20) & 8191;
    if (g_scan_seen[slot] == tag) return;
    g_scan_seen[slot] = tag;
    uint32_t key0[4] = {s->ints, s->sints, g_scan_ti[0], g_scan_tsi[0]}, key1[4] = {0, 0, g_scan_ti[1], g_scan_tsi[1]};
    VStage *vs = p0->st[0].vk, *fs = p1 ? p1->st[1].vk : NULL;
    int fs_made = !p1 || (fs && fs->state == 2 && fs->fo == 0 && key_fits(1, key1, fs->key, fs->dep, fs->tex_units));
    int vs_made = fs_made && vs && vs->state == 2 && key_fits(0, key0, vs->key, vs->dep, vs->tex_units) && vs->fo == (fs && fs->ok ? fs->in_attr & ~(vs->out_attr & ~vs->fo) : 0);
    PKey k = s->sig;
    k.topo_class = tclass[prim]; k.logic = s->logic; k.sshade = p1 ? (uint8_t)p1->sample_shading : 0;
    if (vs_made && fs_made) {
        if (!vs->ok || (fs && !fs->ok)) return;
        k.vs = vs->id; k.fs = fs ? fs->id : 0;
        uint64_t h = pkey_hash(&k);
        EnterCriticalSection(&g_sh_lock);
        Pipe *pf = pipe_find(&k, h);
        if (!pf || pf->state == 0) {               /* not there, or waiting in the background queue: to the front */
            Job *j = calloc(1, sizeof *j);
            j->kind = 1; j->pipe = pf ? pf : pipe_new(&k, vs, fs, h, 0);
            job_push_front(j);
            g_scan_pipes++;
        }
        LeaveCriticalSection(&g_sh_lock);
        return;
    }
    SSrc a, b;
    if (!ssrc_of(&a, p0, 0)) return;
    int has_f = p1 && ssrc_of(&b, p1, 1);
    Job *j = calloc(1, sizeof *j + (size_t)a.code_len + a.cdata_size + 16 + (has_f ? (size_t)b.code_len + b.cdata_size + 16 : 0));
    uint8_t *d = j->data;
    j->kind = 2; j->k = k; memcpy(j->key0, key0, 16); memcpy(j->key1, key1, 16);
    job_copy_src(j, &d, 0, &a);
    if (has_f) job_copy_src(j, &d, 1, &b);
    EnterCriticalSection(&g_sh_lock);
    job_push_front(j);
    g_scan_jobs++;
    LeaveCriticalSection(&g_sh_lock);
}
static void scan_list(uint64_t handle, int depth, Scan *s) {
    size_t len = 0;
    uint8_t *data = nvn_front_list(handle, &len);
    if (!data) return;
    for (size_t off = 0; off + sizeof(Cmd) <= len;) {
        const Cmd *m = (const Cmd *)(data + off);
        const uint64_t *a = m->a;
        if (m->size < sizeof(Cmd)) break;
        switch (m->op) {
        case OP_PROGRAM: for (int st = 0; st < 2; st++) if (a[1] & (1u << st)) { Prog *p = P(a[0]); s->prog[st] = p && !p->dead && p->st[st].state ? p : NULL; }
            if (a[1] & 4u) { Prog *p = P(a[0]); s->gprog = p && !p->dead && p->st[2].state ? p : NULL; }
            break;
        case OP_TARGETS: {
            const TargetRef *col = (const TargetRef *)BLOB(m), *dep = col + 8;
            int n = (int)a[0] < 8 ? (int)a[0] : 8;
            memset(s->col, 0, sizeof s->col); memset(&s->depth, 0, sizeof s->depth); s->ncol = 0;
            for (int i = 0; i < n; i++) if (col[i].tex && !col[i].tex->dead) { s->col[i] = col[i]; s->ncol = i + 1; }
            if (dep->tex && !dep->tex->dead) s->depth = *dep;
            scan_sig(s);
            break;
        }
        case OP_VATTR: s->nattr = (int)a[0] < 32 ? (int)a[0] : 32; memcpy(s->vattr, BLOB(m), (size_t)s->nattr * 4); s->attr_new = 1; break;
        case OP_COLOR: s->logic = ((const uint8_t *)&a[0])[1]; break;
        case OP_DRAW_ARRAYS: case OP_DRAW_ELEMENTS: scan_draw(s, (unsigned)a[0]); break;
        case OP_CALL: for (int i = 0; i < (int)a[0]; i++) { uint64_t h; memcpy(&h, BLOB(m) + i * 8, 8); if (depth < 8) scan_list(h, depth + 1, s); } break;
        default: break;
        }
        off += m->size;
    }
}
/* SMO_VK_MT=1: the look-ahead runs on a thread of its own while the render thread replays the same batch (it used to run
 * first, on the render thread: about 1 ms a frame in New Donk City). It only queues stages and pipelines for the workers;
 * what it reads that the replay may change at the same time (a texture's VTex being made) can at worst make a guess that is
 * not needed. scan_async_wait() before the batch's job returns: the lists stay valid while it reads them. */
static struct { HANDLE thread, go, done; const uint64_t *handles; int count, busy; Scan s; } SA;
static int mt_on(void) { static int on = -1; if (on < 0) { const char *e = getenv("SMO_VK_MT"); on = e && atoi(e) > 0; } return on; }
static DWORD WINAPI scan_thread(void *arg) {
    (void)arg;
    os_pin_host_thread();
    for (;;) {
        WaitForSingleObject(SA.go, INFINITE);
        LONG64 ts = tk();
        for (int i = 0; i < SA.count; i++) scan_list(SA.handles[i], 0, &SA.s);
        g_tm[9] += tk() - ts;
        SetEvent(SA.done);
    }
    return 0;
}
static void scan_async_start(const uint64_t *handles, int count) {
    if (!SA.thread) { SA.go = CreateEventA(NULL, FALSE, FALSE, NULL); SA.done = CreateEventA(NULL, FALSE, FALSE, NULL); SA.thread = CreateThread(NULL, 0, scan_thread, NULL, 0, NULL); }
    scan_begin(&SA.s);
    SA.handles = handles; SA.count = count; SA.busy = 1;
    SetEvent(SA.go);
}
static void scan_async_wait(void) { if (SA.busy) { WaitForSingleObject(SA.done, INFINITE); SA.busy = 0; } }
static void prewarm_wait(void) {                   /* render thread, before its first batch */
    if (g_pw_waited) return;
    g_pw_waited = 1;
    LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
    LONG64 t0 = tk();
    WaitForSingleObject(g_pw_gate, INFINITE);
    printf("nvn_vk: pipelines made ahead: %lld of %d listed (%lld stages from their records, %lld entries without their stage files) in %.0f ms on %d threads; the first batch waited %.0f ms for them\n",
           (long long)g_pw_pipes, g_pw_n, (long long)g_pw_stages, (long long)g_pw_failed, (double)(g_pw_t1 - g_pw_t0) * 1000.0 / (double)fq.QuadPart, g_pw_nthread, (double)(tk() - t0) * 1000.0 / (double)fq.QuadPart);
    fflush(stdout);
    free(g_pw_rec); g_pw_rec = NULL;
}
void nvn_vk_hold_counts(long long *holds, long long *hold_ms, long long *dummies) {     /* smo_nerve.c (SMO_STAGE_LIST): so far */
    LARGE_INTEGER fq;
    QueryPerformanceFrequency(&fq);
    *holds = g_bar_n; *hold_ms = (long long)((double)g_bar_tk * 1000.0 / (double)fq.QuadPart); *dummies = R.dummy_kind;
}

static void pipelines_report(void) {
    LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
    if (D.has_gpl) {
        double q = 1000.0 / (double)fq.QuadPart;
        printf("nvn_vk_gpl: pair libraries compiled by the workers: %lld (%.0f ms of worker time; %lld of them asked for as shared vertex stage x fragment stage without a program of its own: %d such fragment stages, %d shared vertex stages); "
               "PAIR LIBRARY COMPILED AT A DRAW (a hold): %lld, %.1f ms, longest %.2f ms; waited for at a draw or by a worker: %lld, %.1f ms | pipelines LINKED at a draw from ready libraries: %lld (%lld in scene frames), %.1f ms in all, longest %.2f ms; linked by workers: %lld, %.0f ms; %d fragment output libraries\n",
               (long long)g_gpl_made, (double)g_gpl_made_tk * q, (long long)g_gpl_cross, g_nfo, g_nshvs, (long long)g_gpl_draw, (double)g_gpl_draw_tk * q, (double)g_gpl_draw_max * q, (long long)g_gpl_wait, (double)g_gpl_wait_tk * q,
               (long long)g_gpl_dlink, (long long)g_gpl_dlink_scene, (double)g_gpl_dlink_tk * q, (double)g_gpl_dlink_max * q, (long long)g_gpl_link, (double)g_gpl_link_tk * q, g_nolib);
    }
    if (D.has_gpl) printf("nvn_vk_gpl: mode %s; pipelines linked again with optimisation by the workers: %lld of %lld queued, %.0f ms of worker time\n", gpl_sep() ? "2 (a library per stage)" : "1 (a library per stage pair)", (long long)g_gpl_opt_n, (long long)g_gpl_opt_q, (double)g_gpl_opt_tk * 1000.0 / (double)fq.QuadPart);
    printf("nvn_vk_skip: shader_wait = %s; draws left out because a stage or pipeline was not ready: %lld in %lld frames (stage not ready %lld, pipeline not ready %lld)\n", skip_on() ? "skip (near a scene)" : "hold (the default)", (long long)g_skip_n, (long long)g_skip_frames, (long long)g_skip_stage, (long long)g_skip_pipe);
    printf("nvn_vk_barrier: loading-screen frames held for the workers: %lld, %.0f ms in all, longest %.0f ms, time-outs %lld | progress bar: %lld extra presents of a held loading screen\n", (long long)g_bar_n, (double)g_bar_tk * 1000.0 / (double)fq.QuadPart, (double)g_bar_max * 1000.0 / (double)fq.QuadPart, (long long)g_bar_timeout, (long long)g_ovl_presents);
    for (int i = 0; i < g_rtlog_n; i++) printf("nvn_vk_rt: %d x %d (mip level %d), %d colour targets (first format 0x%x), depth format 0x%x: %lld passes, %lld of them in scene frames, first in frame %lld\n", g_rtlog[i].w, g_rtlog[i].h, g_rtlog[i].lv, g_rtlog[i].ncol, g_rtlog[i].cf, g_rtlog[i].df, g_rtlog[i].n, g_rtlog[i].scene, g_rtlog[i].first);
    double k = 1000.0 / (double)fq.QuadPart;
    LONG64 n = 0, t = 0;
    for (int i = 0; i < H_N; i++) { n += g_hold_n[i]; t += g_hold_tk[i]; }
    printf("nvn_vk: texture descriptor sets: %lld reused after a full comparison of their lists, %lld hash hits with a different list (given a set of their own); samplers given a dummy because the texture is of another kind than the shader declares: %lld\n", (long long)R.sets_reused, (long long)R.tset_collisions, (long long)R.dummy_kind);
    if (tc_on()) printf("nvn_vk: texture sets kept across command buffers: %lld allocated (%d pools), %lld written again for another list, %lld lists given a set of the command buffer (every entry of their bucket still in use by the GPU, or the pools full)\n", (long long)g_tc_made, g_tc_npool, (long long)g_tc_rewritten, (long long)g_tc_busy);
    printf("nvn_vk_pipelines: holds of the render thread at a draw: %lld, %.0f ms in all, longest %.1f ms (stage made at the draw %lld / %.0f ms, stage waited for %lld / %.0f ms, pipeline made at the draw %lld / %.0f ms, pipeline waited for %lld / %.0f ms)\n",
           (long long)n, (double)t * k, (double)g_hold_max * k, (long long)g_hold_n[0], (double)g_hold_tk[0] * k, (long long)g_hold_n[1], (double)g_hold_tk[1] * k, (long long)g_hold_n[2], (double)g_hold_tk[2] * k, (long long)g_hold_n[3], (double)g_hold_tk[3] * k);
    printf("nvn_vk_pipelines: workers: %lld programs handed over, %lld stages made by them, %lld pipelines made by them; look-ahead: %lld pipelines and %lld draws with stages still to make sent ahead, %.0f ms of reading in all; blind prediction: %lld keys, %lld used; pass signatures %d; nvnProgramSetShaders held its caller %lld times, %.0f ms in all; background jobs %lld queued / %lld done; workers raised to normal priority for a waiting render thread %lld times\n",
           (long long)g_w_progs, (long long)g_w_stages, (long long)g_w_pipes, (long long)g_scan_pipes, (long long)g_scan_jobs, (double)g_tm[9] * k, (long long)g_pred_made, (long long)g_pred_used, g_nsig, (long long)g_set_held, (double)g_set_held_tk * k, (long long)g_bq_queued, (long long)g_bq_done, (long long)g_boosts);
    for (int i = 0; i < g_hothr_n && i < HOTHR_N; i++)
        printf("nvn_vk_pipelines: programs handed over by thread %s: %lld (%lld of them near a scene), held %lld times, %.0f ms in all\n", g_hothr[i].name ? g_hothr[i].name : "(not a game thread)",
               (long long)g_hothr[i].n, (long long)g_hothr[i].near_scene, (long long)g_hothr[i].held, (double)g_hothr[i].held_tk * k);
}
static void workers_stop(void) {
    if (!g_pw_nthread) return;
    prewarm_wait();
    EnterCriticalSection(&g_sh_lock);
    g_handover_on = 0; g_workers_quit = 1;
    WakeAllConditionVariable(&g_jq_cv);
    LeaveCriticalSection(&g_sh_lock);
    WaitForMultipleObjects((DWORD)g_pw_nthread, g_pw_thread, TRUE, 60000);
    for (int i = 0; i < g_pw_nthread; i++) CloseHandle(g_pw_thread[i]);
    g_pw_nthread = 0;
    pipelines_report();
}
/* ---- the driver's pipeline cache is written on the exit path (vkd_shutdown) and nowhere else. Reading the live cache out
 * (vkGetPipelineCacheData) takes the driver's lock that every pipeline creation and every library link of a draw takes too:
 * done while the game is played, it holds the render thread for as long as the serialisation takes (measured: seconds, a
 * minute on a busy PC, when a thread of lower priority did it during play). One call with a buffer sized from the file of
 * the last save; the two-call form (size, then data) is the fallback. */
static void pcache_save(void) {
    LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
    size_t n = 0;
    void *data = NULL;
    int one = 0, ok = 0;
    if (!g_pcache) return;
    LONG64 t0 = tk();
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExA(g_pcache_path, GetFileExInfoStandard, &fa) && (fa.nFileSizeLow || fa.nFileSizeHigh)) {
        n = (size_t)(((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow);
        n += n / 8 + (64u << 20);
        if ((data = malloc(n)) != NULL) { if (vkGetPipelineCacheData(V.device, g_pcache, &n, data) == VK_SUCCESS) one = 1; else { free(data); data = NULL; } }
    }
    if (!data) {
        n = 0;
        if (vkGetPipelineCacheData(V.device, g_pcache, &n, NULL) != VK_SUCCESS || !n) return;
        if (!(data = malloc(n))) return;
        if (vkGetPipelineCacheData(V.device, g_pcache, &n, data) != VK_SUCCESS) { free(data); return; }
    }
    LONG64 t1 = tk();
    char tmp[176];
    snprintf(tmp, sizeof tmp, "%s.tmp", g_pcache_path);
    FILE *fp = fopen(tmp, "wb");
    if (fp) { ok = fwrite(data, 1, n, fp) == n; fclose(fp); if (ok) ok = MoveFileExA(tmp, g_pcache_path, MOVEFILE_REPLACE_EXISTING) != 0; }
    printf("nvn_vk: pipeline cache %s: %llu KB, on the exit path; vkGetPipelineCacheData %.0f ms (%s); file written in %.0f ms\n",
           ok ? "saved" : "NOT SAVED (the file could not be written)", (unsigned long long)(n >> 10), (double)(t1 - t0) * 1000.0 / (double)fq.QuadPart,
           one ? "one call" : "two calls: size, then data", (double)(tk() - t1) * 1000.0 / (double)fq.QuadPart);
    fflush(stdout);
    free(data);
}
