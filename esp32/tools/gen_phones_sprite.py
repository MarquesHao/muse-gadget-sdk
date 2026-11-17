#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Generate the embedded headphones sprite source for muse_frames.c.

Usage: gen_phones_sprite.py INPUT.png OUTPUT.c

INPUT.png is the 466x466 "happy" pose render: the same front-view
character as the idle sequence, already wearing the headphones. The
headphones (grey headband + two ear cups) are cut out of it, warped
onto the idle close-up's head, and split into three pieces (band,
left cup, right cup) so the frame player's overlay widgets only pay
for the pixels the accessory actually occupies.

Each piece is stored as an LVGL I8 (indexed) image: a 256-entry
BGRA palette followed by one index byte per pixel. That format is a
deliberate budget choice: this board copies .rodata into PSRAM at
boot (CONFIG_SPIRAM_RODATA), so sprite bytes are PSRAM bytes, and
RGB565A8 for these pieces would cost ~200 KB of the scarce pool
against I8's ~68 KB. Native resolution is kept; the per-piece
palette is a median-cut quantization of the piece's own pixels
(index 0 stays fully transparent, alpha is the count-weighted mean
of each entry's members).

Cut method: the fur and skin are warm (channel spread > 40); the
headphones are not (spread < 37) and the background is near-black,
so a saturation cut isolates the phones. Only large connected
components touching the known band/cup cores survive; the mask is
closed, hole-filled and feathered ~1px.

Placement: the happy render is framed slightly differently from the
idle close-up. Anchoring on the head outline (not the face), the
phones layer is scaled by 1.15 about the happy anchor (230,120)
onto the idle anchor (234,118), then shifted (-30,-20) in canvas
space. The warp is applied here, so the emitted pieces are already
in idle-canvas coordinates; muse_frames.c only adds the face
offsets and the wear animation.

The output belongs in the gitignored components/muse/avatar/
directory: the sprite is derived from artwork that ships only on
the user's own device.

Note: the sprite shipped in the 2026-11-17 headphones round was
cut with this method and validated as an RGB565A8 composite first,
then converted to I8 with the same quantizer used here (piece
boxes and pixel shapes unchanged). A fresh from-scratch cut should
be composite-checked against the idle frames before it ships.
"""
import sys
from collections import deque

import numpy as np
from PIL import Image, ImageFilter

W = H = 466
SCALE = 1.15
SRC_ANCHOR = (230, 120)
DST_ANCHOR = (234, 118)
SHIFT = (-30, -20)

# Components smaller than this are mask noise (stray fur pixels).
MIN_COMPONENT = 800


def components(mask):
    seen = np.zeros_like(mask, bool)
    comps = []
    ys, xs = np.nonzero(mask)
    for y0, x0 in zip(ys.tolist(), xs.tolist()):
        if seen[y0, x0]:
            continue
        q = deque([(y0, x0)])
        seen[y0, x0] = True
        pts = []
        while q:
            y, x = q.popleft()
            pts.append((y, x))
            for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                ny, nx = y + dy, x + dx
                if 0 <= ny < H and 0 <= nx < W and mask[ny, nx] and not seen[ny, nx]:
                    seen[ny, nx] = True
                    q.append((ny, nx))
        comps.append(pts)
    return comps


def median_cut(colors, weights, max_boxes):
    """colors: (N,D) unique RGBA float; weights: (N,) counts.
    Returns (palette (M,D), assign (N,) box index per color)."""
    boxes = [(colors, weights, np.arange(len(colors)))]
    while len(boxes) < max_boxes:
        best, best_score = -1, -1.0
        for i, (c, w, idx) in enumerate(boxes):
            if len(c) < 2:
                continue
            rng = (c.max(axis=0) - c.min(axis=0)).astype(np.float64)
            score = float(rng.max() * np.log1p(w.sum()))
            if score > best_score:
                best, best_score = i, score
        if best < 0:
            break
        c, w, idx = boxes.pop(best)
        ch = int(np.argmax(c.max(axis=0) - c.min(axis=0)))
        order = np.argsort(c[:, ch])
        c, w, idx = c[order], w[order], idx[order]
        half = int(np.searchsorted(np.cumsum(w), w.sum() / 2))
        half = min(max(half, 1), len(c) - 1)
        boxes.append((c[:half], w[:half], idx[:half]))
        boxes.append((c[half:], w[half:], idx[half:]))
    palette = np.zeros((len(boxes), colors.shape[1]), np.float64)
    assign = np.zeros(len(colors), int)
    for i, (c, w, idx) in enumerate(boxes):
        palette[i] = (c * w[:, None]).sum(axis=0) / w.sum()
        assign[idx] = i
    return palette, assign


def quantize_piece(rgba):
    """rgba: (h,w,4) uint8, alpha 0 = not part of the piece.
    Returns (data bytes: palette 256xBGRA + index plane, stats).

    Quantization runs in RGBA space, not RGB: the feathered edge has
    many pixels sharing one RGB at very different alphas, and an
    RGB-only palette would smear those alphas across the interior.
    """
    h, w = rgba.shape[:2]
    on = rgba[:, :, 3] > 0
    px = rgba[on]
    colors, inverse, counts = np.unique(px, axis=0,
                                        return_inverse=True,
                                        return_counts=True)
    palette_rgba, assign = median_cut(colors.astype(np.float64),
                                      counts.astype(np.float64), 255)
    entry_of_color = assign + 1  # index 0 = transparent
    palette = np.zeros((256, 4), np.uint8)  # B,G,R,A byte order
    for bi in range(len(palette_rgba)):
        e = bi + 1
        palette[e] = (round(palette_rgba[bi, 2]), round(palette_rgba[bi, 1]),
                      round(palette_rgba[bi, 0]), round(palette_rgba[bi, 3]))
    indices = np.zeros((h, w), np.uint8)
    indices[on] = entry_of_color[inverse]
    pe = entry_of_color[inverse]
    recon = np.stack([palette[pe, 2], palette[pe, 1], palette[pe, 0]],
                     axis=1).astype(np.float64)
    err = np.abs(recon - px[:, :3].astype(np.float64))
    stats = (float(err.mean()), int(err.max()), len(palette_rgba))
    return palette.tobytes() + indices.tobytes(), stats


def main():
    src = np.asarray(Image.open(sys.argv[1]).convert("RGB")).astype(np.int32)
    r, g, b = src[:, :, 0], src[:, :, 1], src[:, :, 2]
    spread = src.max(axis=2) - src.min(axis=2)
    lum = (r + g + b) // 3
    mask = (spread < 37) & (lum > 45)
    # Restrict to the headphone zone so dark non-warm clutter elsewhere
    # (eyes, mouth, outline shadows) cannot join.
    zone = np.zeros_like(mask)
    zone[30:280, 40:420] = True
    mask &= zone
    # Cores the real parts must touch: band top + both cups.
    cores = np.zeros_like(mask)
    cores[40:60, 200:280] = True
    cores[180:230, 95:130] = True
    cores[165:215, 345:380] = True
    keep = np.zeros_like(mask)
    for pts in components(mask):
        if len(pts) < MIN_COMPONENT:
            continue
        if not any(cores[y, x] for y, x in pts):
            continue
        for y, x in pts:
            keep[y, x] = True
    # Close small gaps + fill holes via binary morphology.
    mimg = Image.fromarray((keep * 255).astype(np.uint8), "L")
    mimg = mimg.filter(ImageFilter.MaxFilter(5)).filter(ImageFilter.MinFilter(5))
    keep = np.asarray(mimg) > 0
    # Warp mask + colour into idle-canvas space.
    inv = Image.fromarray((keep * 255).astype(np.uint8), "L")
    col = Image.open(sys.argv[1]).convert("RGB")
    size = (round(W * SCALE), round(H * SCALE))
    inv_big = inv.resize(size, Image.LANCZOS)
    col_big = col.resize(size, Image.LANCZOS)
    ox = round(DST_ANCHOR[0] - SRC_ANCHOR[0] * SCALE) + SHIFT[0]
    oy = round(DST_ANCHOR[1] - SRC_ANCHOR[1] * SCALE) + SHIFT[1]
    alpha_big = np.zeros((H, W), np.float32)
    col_arr = np.zeros((H, W, 3), np.uint8)
    tmpa = np.asarray(inv_big).astype(np.float32)
    ca = np.asarray(col_big)
    ys = slice(max(0, oy), min(H, oy + size[1]))
    xs = slice(max(0, ox), min(W, ox + size[0]))
    bys = slice(ys.start - oy, ys.stop - oy)
    bxs = slice(xs.start - ox, xs.stop - ox)
    alpha_big[ys, xs] = tmpa[bys, bxs]
    col_arr[ys, xs] = ca[bys, bxs]
    alpha = np.clip(alpha_big / 255.0, 0, 1)
    aimg = Image.fromarray((alpha * 255).astype(np.uint8), "L").filter(
        ImageFilter.GaussianBlur(1.1))
    alpha = np.asarray(aimg).astype(np.float32) / 255.0
    rgba = np.dstack([col_arr, (alpha * 255).astype(np.uint8)])
    # Split into pieces: band / left cup / right cup.
    on = alpha > 0.004
    boxes = [(40, 20, 426, 140, "band"), (30, 115, 150, 300, "cup_l"),
             (330, 105, 445, 275, "cup_r")]
    pieces = []
    used = np.zeros_like(on)
    for x0, y0, x1, y1, name in boxes:
        sub = np.zeros_like(on)
        sub[y0:y1, x0:x1] = on[y0:y1, x0:x1]
        sub &= ~used
        if not sub.any():
            continue
        ys2, xs2 = np.nonzero(sub)
        bx0, bx1 = int(xs2.min()), int(xs2.max()) + 1
        by0, by1 = int(ys2.min()), int(ys2.max()) + 1
        crop = rgba[by0:by1, bx0:bx1].copy()
        crop[~sub[by0:by1, bx0:bx1]] = 0
        used |= sub
        data, stats = quantize_piece(crop)
        pieces.append((name, bx0, by0, bx1 - bx0, by1 - by0, data))
        print(f"{name}: box ({bx0},{by0}) {bx1-bx0}x{by1-by0}, "
              f"data {len(data)} B, palette entries {stats[2]}, "
              f"RGB err mean {stats[0]:.2f} max {stats[1]}")
    leftover = int((on & ~used).sum())
    print(f"visible px dropped outside pieces: {leftover}")
    with open(sys.argv[2], "w") as f:
        f.write("/* GENERATED by tools/gen_phones_sprite.py from the\n"
                " * on-device artwork (happy still). Do not edit.\n"
                " * LVGL I8: 256-entry BGRA palette + index plane.\n"
                " * Local build only: derived from artwork that ships\n"
                " * on the user's own device, never committed. */\n")
        f.write('#include "muse_phones_sprite.h"\n\n')
        for name, x, y, w, h, data in pieces:
            f.write(f"static const uint8_t s_{name}[] = {{\n")
            for i in range(0, len(data), 20):
                f.write(",".join(str(v) for v in data[i:i + 20]) + ",\n")
            f.write("};\n\n")
        f.write("static const muse_phones_piece_t s_pieces[] = {\n")
        for name, x, y, w, h, data in pieces:
            f.write(f"    {{ s_{name}, {len(data)}, {w}, {h}, {x}, {y} }},\n")
        f.write("};\n\n")
        f.write("const muse_phones_piece_t *muse_phones_pieces(size_t *count_out)\n"
                "{\n    *count_out = sizeof(s_pieces) / sizeof(s_pieces[0]);\n"
                "    return s_pieces;\n}\n")
    total = sum(len(p[5]) for p in pieces)
    print(f"total sprite data: {total} bytes")


if __name__ == "__main__":
    main()
