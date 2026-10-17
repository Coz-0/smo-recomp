/* nvn_vk_store.c - SMO port: the CONTAINER STORE (included by nvn_vk_m4.c; not a build unit of its own).
 *
 * In ssbu-recomp this file scanned Smash's data.arc with Smash's own zstd decoder (two 13.0.4 function addresses) and
 * wrote every "BNSH" shader container it found into the store folder, so the preparation threads could build pipelines
 * ahead of the game. SMO has no data.arc: its shaders live in SARC / Yaz0 archives of the RomFS (ShaderData/, effect and
 * layout archives). Until that reader exists, the store is built EMPTY: nvn_vk_m4.c then reports
 * "NO PREPARATION" and every container the game hands over is translated and compiled at its first draw, which is the
 * renderer's normal fallback (slower first frames, same pictures). */

static int vks_build(const char *dir) {
    CreateDirectoryA(dir, NULL);
    printf("nvn_vk_store: the SMO shader-container store is not implemented yet: nothing prepared ahead in %s\n", dir);
    return 0;
}
