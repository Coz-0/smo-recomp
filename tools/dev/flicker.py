"""Find flicker in consecutive frame captures (SMO_SHOT_EVERY=1 with SMO_SHOT_FROM / SMO_SHOT_TO).

    python tools/dev/flicker.py <folder with frame_<n>.bmp> [--out <folder>] [--rows 4] [--cols 4] [--threshold 6]
                                [--pixels]

A flash: the mean brightness of a region of the picture (a --rows x --cols grid) in frame n differs from frames n-1 and
n+1 by more than the threshold (0..255 levels) in the same direction, while n-1 and n+1 are within half the threshold of
each other. Something that shows in one frame only; region means change smoothly when the camera moves, so a camera
pan does not count. Prints every flash (frame, regions, how far it went) and writes, for the largest ones, the three
frames side by side (flash_<n>.png) into --out.

--pixels: also the pixel test (a pixel that changes by more than 40 and back within three frames), written as a heat
map (flicker_heat.png). It is fooled by motion of fine detail (grass) and suits captures with a still camera.
"""
import argparse
import os
import re

import numpy as np
from PIL import Image


def load(path, scale=1):
    im = Image.open(path).convert('RGB')
    if scale > 1:
        im = im.resize((im.size[0] // scale, im.size[1] // scale), Image.BILINEAR)
    return np.asarray(im, dtype=np.int16)


def region_means(img, rows, cols):
    lum = img.mean(axis=2)
    h, w = lum.shape
    return np.array([[lum[r * h // rows:(r + 1) * h // rows, c * w // cols:(c + 1) * w // cols].mean() for c in range(cols)] for r in range(rows)])


def sheet(folder, f, out):
    trip = [Image.open(os.path.join(folder, 'frame_%d.bmp' % k)).convert('RGB') for k in (f - 1, f, f + 1)]
    w, h = trip[0].size
    s = Image.new('RGB', (w * 3 // 2, h // 2))
    for k, im in enumerate(trip):
        s.paste(im.resize((w // 2, h // 2)), (k * w // 2, 0))
    s.save(os.path.join(out, 'flash_%d.png' % f))


def pixel_test(folder, frames, out, threshold=40, scale=4):
    imgs = [load(os.path.join(folder, frames[0][1]), scale), load(os.path.join(folder, frames[1][1]), scale)]
    heat = np.zeros(imgs[0].shape[:2], dtype=np.int32)
    for i in range(2, len(frames)):
        f2 = load(os.path.join(folder, frames[i][1]), scale)
        if frames[i][0] - frames[i - 2][0] == 2:
            f0, f1 = imgs
            d1 = np.abs(f1 - f0).max(axis=2)
            d2 = np.abs(f2 - f1).max(axis=2)
            d02 = np.abs(f2 - f0).max(axis=2)
            heat += (d1 > threshold) & (d2 > threshold) & (d02 < threshold // 2)
        imgs = [imgs[1], f2]
    if heat.max() > 0:
        base = load(os.path.join(folder, frames[len(frames) // 2][1]), scale).astype(np.float32) * 0.4
        base[..., 0] = np.maximum(base[..., 0], 255.0 * heat / heat.max())
        Image.fromarray(base.clip(0, 255).astype(np.uint8)).save(os.path.join(out, 'flicker_heat.png'))
        print('pixel test: heat map %s' % os.path.join(out, 'flicker_heat.png'))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('folder')
    ap.add_argument('--out')
    ap.add_argument('--rows', type=int, default=4)
    ap.add_argument('--cols', type=int, default=4)
    ap.add_argument('--threshold', type=float, default=6.0)
    ap.add_argument('--sheets', type=int, default=6, help='side-by-side pictures of the largest flashes')
    ap.add_argument('--pixels', action='store_true')
    a = ap.parse_args()
    out = a.out or a.folder
    os.makedirs(out, exist_ok=True)
    frames = sorted((int(m.group(1)), f) for f in os.listdir(a.folder) for m in [re.match(r'frame_(\d+)\.bmp$', f)] if m)
    if len(frames) < 3:
        raise SystemExit('need at least 3 frames, found %d' % len(frames))
    gaps = sum(1 for x, b in zip(frames, frames[1:]) if b[0] - x[0] != 1)
    print('%d frames, %d..%d%s' % (len(frames), frames[0][0], frames[-1][0], ', %d gaps' % gaps if gaps else ''))
    means = {n: region_means(load(os.path.join(a.folder, f)), a.rows, a.cols) for n, f in frames}
    t = a.threshold
    flashes = []
    for n, _ in frames:
        if n - 1 not in means or n + 1 not in means:
            continue
        p, c, q = means[n - 1], means[n], means[n + 1]
        d1, d2, d02 = c - p, c - q, np.abs(q - p)
        hit = ((d1 > t) & (d2 > t) | (d1 < -t) & (d2 < -t)) & (d02 < t / 2)
        if hit.any():
            size = float(np.abs(np.where(hit, (d1 + d2) / 2, 0)).max())
            cells = ['r%dc%d %+.0f' % (r, k, (d1[r, k] + d2[r, k]) / 2) for r in range(a.rows) for k in range(a.cols) if hit[r, k]]
            flashes.append((size, n, cells))
    print('%d frames with a flash (a region brighter or darker than both neighbours by more than %.0f levels):' % (len(flashes), t))
    for size, n, cells in sorted(flashes, key=lambda x: x[1]):
        print('  frame %d: %d of %d regions, up to %.0f levels: %s' % (n, len(cells), a.rows * a.cols, size, ', '.join(cells)))
    for size, n, cells in sorted(flashes, reverse=True)[:a.sheets]:
        sheet(a.folder, n, out)
    if flashes:
        print('largest flashes side by side (frames n-1, n, n+1): flash_<n>.png in %s' % out)
    if a.pixels:
        pixel_test(a.folder, frames, out)


if __name__ == '__main__':
    main()
