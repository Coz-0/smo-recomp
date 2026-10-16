/* nvn_layout_smo.c - SMO port: the NVN texture-storage answers of nvn_sdk.h, computed instead of lifted.
 *
 * ssbu-recomp answered these with NVN's own code lifted out of Smash's sdk module (13.0.4 contains the NVN driver).
 * SMO 1.0.0's sdk (NintendoSDK 3.5.1) does NOT contain the driver: only nvnBootstrapLoader is there (4 NVN symbols, Phase
 * 1 function list); the console supplied the driver. So nothing can be lifted, and the answers come from the layout
 * engine of tex_decode.c (tex_layout), the same rules ssbu-recomp's own texture code used.
 *
 * Verified (tools/test/bntx_layout_check.py, 2026-10-15): for all 19,325 textures in SMO's RomFS (4,335 archives) the
 * storage size, the block height and the offset of every mip level computed here are equal to the values Nintendo's
 * tools stored in the BNTX texture records. What that check does NOT cover, and how it is answered:
 *   - alignment: BNTX stores 512 for every texture (the container's data alignment), so it is no reference. The value
 *     here is tex_layout's rule, 512 << (block height log2 + block depth log2): one block of level 0, as ssbu-recomp read
 *     it in the 13.0.4 driver. It is always a multiple of 512.
 *   - multisample: size x samples (ssbu-recomp nvn_gl.c tex_storage_size2); no SMO reference yet.
 *   - linear (pitch) textures: stride x height (same source); no SMO reference yet.
 *   - buffer textures (target 10): size = width x bytes per element, alignment = bytes per element with 12 counted as 4
 *     (ssbu-recomp's reading of the 13.0.4 driver at [sdk 0x4278d8 / 0x427fe8]); no SMO reference yet.
 *   - storage class: 0x100 for every texture; why that cannot be misused is at nvn_sdk_texture_storage_class below.
 *   - raw storage class, sparse tile layout: NOT ANSWERED. A call stops the program with the function's name (no
 *     reference for SMO's driver version yet; a guess could misplace textures in memory pools silently). */
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include "hostrt.h"
#include "nvn_sdk.h"
#include "tex_decode.h"

int nvn_sdk_format_known(uint32_t format) { return tex_format((int)format) != NULL; }

static uint64_t buffer_bpe(uint32_t format) {
    const TexFormat *f = tex_format((int)format);
    return f ? (uint64_t)f->bpb : 4;
}

uint64_t nvn_sdk_texture_storage_size(Ctx *c, const NvnSdkTexBuilder *b) {
    (void)c;
    if (b->target == 10) return (uint64_t)(b->width ? b->width : 1) * buffer_bpe(b->format);
    if (b->flags & 0x10) {                                     /* NVN_TEXTURE_FLAGS_LINEAR */
        uint64_t row = b->stride ? b->stride : (uint64_t)(b->width ? b->width : 1) * buffer_bpe(b->format);
        return row * (b->height ? b->height : 1);
    }
    TexLayout L;
    tex_layout(&L, (int)b->target, (int)b->format, (int)b->width, (int)b->height, (int)b->depth, (int)b->levels,
               (int)(b->flags & ~0x50u), 0, b->has_layout && (b->layout & 0xff) <= 4 ? (int)(b->layout & 0xff) : -1);
    return (uint64_t)L.size * (b->samples > 1 ? b->samples : 1);
}

uint64_t nvn_sdk_texture_storage_alignment(Ctx *c, const NvnSdkTexBuilder *b) {
    (void)c;
    if (b->target == 10) { uint64_t e = buffer_bpe(b->format); return e == 12 ? 4 : e; }
    TexLayout L;
    tex_layout(&L, (int)b->target, (int)b->format, (int)b->width, (int)b->height, (int)b->depth, (int)b->levels,
               (int)b->flags, b->stride, b->has_layout && (b->layout & 0xff) <= 4 ? (int)(b->layout & 0xff) : -1);
    return (uint64_t)L.alignment;
}

static void not_answered(Ctx *c, const char *what) {
    printf("\nnvn: %s has no SMO answer yet (no reference for SMO's NVN driver version): stopping (see runtime/nvn_layout_smo.c)\n", what);
    guest_trap("NVN storage query without an SMO answer", c ? c->x[30] - 4 : 0);
}
/* Storage class = 0x100 | GPU memory kind (ssbu-recomp's reading of the 13.0.4 driver). Its only consumer in NVN is
 * nvnMemoryPoolMapVirtual (a mapping request carries it; the driver passes class & ~0x100 to the GPU address-space remap,
 * nvn_stub.c). That function has NO handler in this port, so a call stops with its name: the kind can never be used.
 * In SMO the value is read by agl::detail::Surface::calcSizeAndAlignment [game 0x7b703c] and stored at Surface +0x18;
 * no agl function loads it back (scan of all 3,382 agl functions, 2026-10-15). The answer is therefore 0x100 (the
 * class with kind 0, which the driver gives linear textures) for every texture: a value that cannot be misused here. */
uint32_t nvn_sdk_texture_storage_class(Ctx *c, const NvnSdkTexBuilder *b) { (void)c; (void)b; return 0x100; }
uint32_t nvn_sdk_texture_raw_storage_class(Ctx *c, const NvnSdkTexBuilder *b) { (void)b; not_answered(c, "nvnTextureBuilderGetRawStorageClass"); return 0; }
void nvn_sdk_texture_sparse_layout(Ctx *c, const NvnSdkTexBuilder *b, uint32_t out[4]) { (void)b; (void)out; not_answered(c, "nvnTextureBuilderGetSparseTileLayout"); }
int nvn_sdk_tex_storage_class(Ctx *c, const NvnSdkTexBuilder *b, uint32_t *out) { *out = nvn_sdk_texture_storage_class(c, b); return 1; }
int nvn_sdk_tex_raw_storage_class(Ctx *c, const NvnSdkTexBuilder *b, uint32_t *out) { (void)b; (void)out; not_answered(c, "nvnTextureGetRawStorageClass"); return 0; }
int nvn_sdk_tex_sparse_layout(Ctx *c, const NvnSdkTexBuilder *b, uint32_t out[4]) { (void)b; (void)out; not_answered(c, "nvnTextureGetSparseTileLayout"); return 0; }
int nvn_sdk_tex_storage_size(Ctx *c, const NvnSdkTexBuilder *b, uint32_t *out) { *out = (uint32_t)nvn_sdk_texture_storage_size(c, b); return 1; }

int nvn_sdk_selftest(Ctx *c) {
    (void)c;
    printf("  nvn storage: computed for SMO (no lifted driver); verified offline by tools/test/bntx_layout_check.py\n");
    return 0;
}
