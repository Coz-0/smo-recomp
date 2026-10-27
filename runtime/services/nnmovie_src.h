/* The demuxing and decoding reader under nnmovie_mf.c's movie:: protocol: Media Foundation's source reader on Windows
 * (nnmovie_mf.c), AVFoundation's asset reader on macOS (nnmovie_av.m). Both read a whole file held in memory, demux AND
 * decode, and hand out decoded samples one track at a time: packed NV12 video (Y rows, then interleaved UV rows, w bytes
 * each) and interleaved 16-bit PCM. A sample is opaque to the protocol code. Not thread-safe: the extractor's lock is held. */
#ifndef NNMOVIE_SRC_H
#define NNMOVIE_SRC_H
#include <stddef.h>
#include <stdint.h>

#define MV_MAX_TRACKS 8
typedef struct {
    int kind;                                         /* 0 video, 1 audio, 2 other */
    char mime[48];                                    /* the stagefright names: "video/avc", "audio/mp4a-latm", ... */
    int w, h, rate, ch;
    double fps;
} MvTrack;
typedef struct MvSrc MvSrc;

/* path: the game's name of the file (its extension says the container). NULL when it cannot be opened (printed). */
MvSrc *mv_open(const unsigned char *data, size_t size, const char *path, int64_t *duration_us, MvTrack *tracks, int *ntracks);
/* on: decode the track from now on (NV12 / 16-bit PCM; the track's size, rate and channels are updated to the output's);
 * 0 ok, -1 no decoder for it (left unselected, printed). */
int mv_select(MvSrc *s, int track, int on, MvTrack *t);
/* the next decoded sample of a selected track: 1 with *sample and its time, 0 at its end, -1 when decoding failed */
int mv_read(MvSrc *s, int track, void **sample, int64_t *us);
int mv_seek(MvSrc *s, int64_t us);                    /* 0 ok; every track reads from there */
void mv_release(void *sample);
/* copies a sample into dst: the bytes written (0 when it does not fit, for video); *need = the sample's whole size */
size_t mv_copy(void *sample, int is_video, int w, int h, unsigned char *dst, size_t cap, size_t *need);
void mv_close(MvSrc *s);
#endif
