"""Check of runtime/gfx/glyph_icons.c on the game's own button glyph sheet, without running the game.

The prompts of play draw their button icons from DeviceFont80.bffnt (LocalizedData/<language>/LayoutData/FontData.szs,
the same file in every language). Its glyph sheet is a BNTX inside the font: a BC4 texture of 512 x 1024. This takes it
out of the romfs of build/, deswizzles it as the renderer does, builds a small program around glyph_icons.c and checks:
the sheet is recognised (and not with another level count or size), the Xbox style trades the A / B and X / Y cells
(within BC4's rounding), and nothing else of the sheet changes.

    python tests/check_button_icons.py      (after scripts/setup.py has extracted the romfs)
"""

import os
import struct
import subprocess
import sys
import tempfile

import numpy as np

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools"))
sys.path.insert(0, os.path.join(REPO, "tools", "dev"))
import target as T
from sarc import yaz0

FONT = os.path.join(
    T.build("romfs"), "LocalizedData", "USen", "LayoutData", "FontData.szs"
)
CELLS = {
    8: 9,
    9: 8,
    10: 11,
    11: 10,
}  # U+E0E0..E3 (A B X Y): the cell each one takes its picture from in the Xbox style
TEST_C = r"""
#include <stdio.h>
#include "glyph_icons.h"
int glyph_icons_style(void) { return GLYPH_ICONS_NINTENDO; }
int main(int argc, char **argv) {
    static uint8_t s[262144];
    FILE *f = argc == 3 ? fopen(argv[1], "rb") : NULL;
    if (!f || fread(s, 1, sizeof s, f) != sizeof s) return 2;
    fclose(f);
    printf("%d %d %d %d %d\n", glyph_icons_is_sheet(512, 1024, 1, 1, s), glyph_icons_is_sheet(512, 1024, 2, 1, s),
           glyph_icons_is_sheet(512, 512, 1, 1, s), glyph_icons_apply(s, GLYPH_ICONS_NINTENDO), glyph_icons_apply(s, GLYPH_ICONS_XBOX));
    f = fopen(argv[2], "wb");
    return !f || fwrite(s, 1, sizeof s, f) != sizeof s || fclose(f);
}
"""


def sarc_file(d, want):
    hdr, data_off = (
        struct.unpack_from("<H", d, 4)[0],
        struct.unpack_from("<I", d, 0xC)[0],
    )
    nodes = struct.unpack_from("<H", d, hdr + 6)[0]
    names = hdr + 0xC + nodes * 0x10 + 8
    for i in range(nodes):
        _, attr, beg, end = struct.unpack_from("<IIII", d, hdr + 0xC + i * 0x10)
        o = names + (attr & 0xFFFF) * 4
        if d[o : d.index(b"\0", o)].decode() == want:
            return d[data_off + beg : data_off + end]
    sys.exit(f"check_button_icons: {want} not in {FONT}")


def sheet(font):
    """the BC4 level of the glyph sheet, deswizzled (Tegra block linear, the block height of its BRTI)"""
    b = font.find(b"BNTX")
    t = font.find(b"BRTI", b)
    fmt, w, h, bh = (
        struct.unpack_from("<BBHHHIIIIIIIII", font, t + 0x10)[i] for i in (6, 8, 9, 12)
    )
    if fmt >> 8 != 0x1D or (w, h) != (512, 1024):
        sys.exit(
            f"check_button_icons: the sheet is format {fmt:#x}, {w} x {h}: not the BC4 512 x 1024 expected"
        )
    size = struct.unpack_from("<I", font, t + 0x50)[0]
    off = (
        b
        + struct.unpack_from(
            "<Q", font, b + struct.unpack_from("<Q", font, t + 0x70)[0]
        )[0]
    )
    src = np.frombuffer(font[off : off + size], np.uint8)
    row_bytes, rows = w // 4 * 8, h // 4
    y, xb = np.mgrid[0:rows, 0:row_bytes]
    gy, x, yy = y // 8, xb % 64, y % 8
    o = ((gy >> bh) * (row_bytes // 64) + xb // 64) * (512 << bh) + (
        gy & ((1 << bh) - 1)
    ) * 512
    o += (
        ((x & 32) << 3) | ((yy & 6) << 5) | ((x & 16) << 1) | ((yy & 1) << 4) | (x & 15)
    )
    return src[o].tobytes()


def decode(raw):
    """BC4 512 x 1024 (linear blocks) -> 8-bit pixels"""
    a = np.frombuffer(raw, np.uint8).reshape(256, 128, 8).astype(np.int32)
    a0, a1 = a[..., 0], a[..., 1]
    bits = sum(a[..., 2 + k].astype(np.int64) << (8 * k) for k in range(6))
    pal = np.zeros((256, 128, 8), np.int32)
    pal[..., 0], pal[..., 1] = a0, a1
    gt = a0 > a1
    for k in range(1, 7):
        pal[..., 1 + k] = np.where(gt, ((7 - k) * a0 + k * a1) // 7, pal[..., 1 + k])
    for k in range(1, 5):
        pal[..., 1 + k] = np.where(~gt, ((5 - k) * a0 + k * a1) // 5, pal[..., 1 + k])
    pal[..., 6], pal[..., 7] = (
        np.where(~gt, 0, pal[..., 6]),
        np.where(~gt, 255, pal[..., 7]),
    )
    img = np.zeros((1024, 512), np.int32)
    for p in range(16):
        idx = (bits >> (3 * p)) & 7
        img[p // 4 :: 4, p % 4 :: 4] = np.take_along_axis(pal, idx[..., None], 2)[
            ..., 0
        ]
    return img


def cell(img, n):
    x, y = (n % 6) * 81, 1024 - (n // 6) * 85 - 84
    return img[y : y + 84, x : x + 80]


def main():
    if not os.path.exists(FONT):
        sys.exit(
            f"check_button_icons: {FONT} not found (scripts/setup.py extracts the romfs)"
        )
    with open(FONT, "rb") as f:
        d = f.read()
    lin = sheet(sarc_file(yaz0(d) if d[:4] == b"Yaz0" else d, "DeviceFont80.bffnt"))
    with tempfile.TemporaryDirectory() as tmp:
        src, exe, a, b = (
            os.path.join(tmp, n) for n in ("t.c", "t", "in.bc4", "out.bc4")
        )
        with open(src, "w") as f:
            f.write(TEST_C)
        with open(a, "wb") as f:
            f.write(lin)
        cmd = [
            T.clang(),
            "-O1",
            "-Wall",
            "-I",
            os.path.join(REPO, "runtime", "gfx"),
            src,
            T.runtime("gfx", "glyph_icons.c"),
            "-o",
            exe,
        ]
        if subprocess.run(cmd, check=False).returncode:
            sys.exit("check_button_icons: build failed")
        r = subprocess.run([exe, a, b], check=False, capture_output=True, text=True)
        flags = r.stdout.split()
        with open(b, "rb") as f:
            out = f.read()
    fails = []
    if r.returncode or flags != ["1", "0", "0", "0", "1"]:
        fails.append(
            f"is_sheet (1 level, 2 levels, 512 x 512) and apply (Nintendo, Xbox) gave {flags}, expected 1 0 0 0 1"
        )
    before, after = decode(lin), decode(out)
    for n, src_n in CELLS.items():
        err = np.abs(cell(after, n) - cell(before, src_n)).max()
        if err > 32:
            fails.append(f"cell {n}: {err} from the picture of cell {src_n}")
    keep = np.ones(before.shape, bool)
    for n in CELLS:
        cell(keep, n)[:] = False
    if (before != after)[keep].any():
        fails.append(
            f"{int((before != after)[keep].sum())} pixels outside the traded cells changed"
        )
    for line in fails:
        print("FAIL", line)
    print(
        "check_button_icons: %s"
        % (
            "FAILED"
            if fails
            else "sheet recognised; A/B and X/Y traded for Xbox; the rest unchanged"
        )
    )
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
