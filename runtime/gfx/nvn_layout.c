/* nvn_layout.c - the NVN texture-storage answers of nvn_sdk.h (size, alignment, block height, mip offsets), computed.
 *
 * The game's SDK module does not contain the NVN driver: it defines three NVN symbols (nvnBootstrapLoader,
 * nvnDevtoolsBootstrap, nvnBootstrapLoaderInternal) and the console supplies the driver. So there is no driver code to
 * lift, and the answers come from the layout engine of tex_decode.c (tex_layout).
 *
 * Verified (tests/bntx_layout_check.py): for every texture in the game's RomFS (20,203 in 4,666 archives) the storage
 * size, the block height and the offset of every mip level computed here are equal to the values Nintendo's tools
 * stored in the BNTX texture records. What that check does NOT cover, and how it is answered:
 *   - alignment: BNTX stores 512 for every texture (the container's data alignment), so it is no reference. The value
 *     here is tex_layout's rule, 512 << (block height log2 + block depth log2): one block of level 0, as ssbu-recomp read
 *     it in the NVN driver of Super Smash Bros. Ultimate 13.0.4. It is always a multiple of 512.
 *   - multisample: size x samples (ssbu-recomp's rule); not checked against this game.
 *   - linear (pitch) textures: stride x height (same source); not checked against this game.
 *   - buffer textures (target 10): size = width x bytes per element, alignment = bytes per element with 12 counted as 4
 *     (ssbu-recomp's reading of the same driver at [sdk 0x4278d8 / 0x427fe8]); not checked against this game.
 *   - storage class: 0x100 for every texture; why that cannot be misused is at nvn_sdk_texture_storage_class below.
 *   - raw storage class, sparse tile layout: NOT ANSWERED. A call stops the program with the function's name (no
 *     reference for this game's driver version; a guess could misplace textures in memory pools silently). */
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include "win32_posix.h"
#endif
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
    printf("\nnvn: %s has no SMO answer yet (no reference for SMO's NVN driver version): stopping (see runtime/nvn_layout.c)\n", what);
    guest_trap("NVN storage query without an SMO answer", c ? c->x[30] - 4 : 0);
}
/* Storage class = 0x100 | GPU memory kind (ssbu-recomp's reading of Super Smash Bros. Ultimate 13.0.4's NVN driver). Its only consumer in NVN is
 * nvnMemoryPoolMapVirtual (a mapping request carries it; the driver passes class & ~0x100 to the GPU address-space remap,
 * nvn_stub.c), and the runtime's nvnMemoryPoolMapVirtual has no GPU memory kind to apply: it only prints the class.
 * The game reads the value in agl::detail::Surface::calcSizeAndAlignment (base game: [game 0x7b703c]) and stores it at
 * Surface +0x18; no agl function loads it back (a scan of all 3,382 agl functions of the base game, which has symbols).
 * The answer is therefore 0x100 (the
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
    printf("  nvn storage: computed for SMO (no lifted driver); verified offline by tests/bntx_layout_check.py\n");
    return 0;
}
