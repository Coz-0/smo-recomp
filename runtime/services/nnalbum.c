/* The album: pictures of the Capture button, kept in <userdata>\album (<userdata> = %SMO_USERDATA% or ..\userdata) as
 * <date>_<time>_<nn>.bmp (32-bit BMP; a console stores JPEG). The game imports no nn::album function. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *fs_user_root(void);                           /* nnfs.c: %SMO_USERDATA% or ..\userdata */
static char g_album_dir[MAX_PATH];
static const char *album_dir(void) {
    if (!g_album_dir[0]) snprintf(g_album_dir, sizeof g_album_dir, "%s\\album", fs_user_root());
    return g_album_dir;
}
static volatile LONG g_shot_counter;

/* Writes a top-down RGBA8 picture as a 32-bit BMP (BGRA, bottom-up). Returns 0 on success. */
static int write_bmp(const char *path, const uint8_t *rgba, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return 1;
    BITMAPFILEHEADER fh;
    BITMAPINFOHEADER ih;
    memset(&fh, 0, sizeof fh);
    memset(&ih, 0, sizeof ih);
    fh.bfType = 0x4d42;
    fh.bfOffBits = sizeof fh + sizeof ih;
    fh.bfSize = fh.bfOffBits + (DWORD)w * (DWORD)h * 4;
    ih.biSize = sizeof ih; ih.biWidth = w; ih.biHeight = h; ih.biPlanes = 1; ih.biBitCount = 32; ih.biCompression = BI_RGB;
    fwrite(&fh, sizeof fh, 1, f);
    fwrite(&ih, sizeof ih, 1, f);
    uint8_t *row = malloc((size_t)w * 4);
    for (int y = h - 1; y >= 0; y--) {
        const uint8_t *s = rgba + (size_t)y * w * 4;
        for (int x = 0; x < w; x++) { row[x * 4] = s[x * 4 + 2]; row[x * 4 + 1] = s[x * 4 + 1]; row[x * 4 + 2] = s[x * 4]; row[x * 4 + 3] = s[x * 4 + 3]; }
        fwrite(row, 4, (size_t)w, f);
    }
    free(row);
    int bad = ferror(f);
    fclose(f);
    return bad;
}
/* a picture of the Capture button (nvn_vk_draw.c capture_vk): the presented frame, w x h opaque 8-bit RGBA,
 * saved as <album>\<date>_<time>_<n>.bmp and turned as the game last asked with
 * nn::oe::SetScreenShotImageOrientation (smo_nn.c): 1 = 90, 2 = 180, 3 = 270 degrees clockwise (NOT VERIFIED on a
 * console: the direction of the turn). */
int smo_screenshot_orientation(void);                     /* smo_nn.c */
int album_save_capture(const uint8_t *rgba, int w, int h, char *out_path, size_t out_cap) {
    int o = smo_screenshot_orientation() & 3, ow = (o & 1) ? h : w, oh = (o & 1) ? w : h;
    uint8_t *turned = NULL;
    const uint8_t *pix = rgba;
    if (o) {
        turned = malloc((size_t)w * h * 4);
        if (!turned) return 1;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int nx = o == 1 ? h - 1 - y : o == 2 ? w - 1 - x : y, ny = o == 1 ? x : o == 2 ? h - 1 - y : w - 1 - x;
                memcpy(turned + ((size_t)ny * ow + nx) * 4, rgba + ((size_t)y * w + x) * 4, 4);
            }
        pix = turned;
    }
    SYSTEMTIME t;
    char path[MAX_PATH];
    GetLocalTime(&t);
    CreateDirectoryA(album_dir(), NULL);
    snprintf(path, sizeof path, "%s\\%04d%02d%02d_%02d%02d%02d_%02ld.bmp", album_dir(), t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
             InterlockedIncrement(&g_shot_counter) % 100);
    int bad = write_bmp(path, pix, ow, oh);
    if (out_path && out_cap) snprintf(out_path, out_cap, "%s", path);
    if (bad) printf("capture: cannot write %s\n", path);
    else printf("capture: %dx%d picture%s saved to %s\n", ow, oh, o ? " (turned as the game asked)" : "", path);
    free(turned);
    return bad;
}
