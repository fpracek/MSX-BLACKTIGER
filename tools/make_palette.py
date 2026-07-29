#!/usr/bin/env python3
"""Level-1 16-color palette: tile-frequency background + reserved hero grays.

Background entries come from the map-window tiles weighted by on-screen
frequency (the recipe that produced the approved dark cave look). Layout:
0 = black (backdrop/transparent), 1 = pure black (curtain/outlines),
2..13 = background, 14,15 = hero armor grays. Writes rip/level1_pal.json.
"""
import json
from collections import Counter
from pathlib import Path
import numpy as np
from decode_gfx import load_bg_tiles
from render_rip import load_palette, scan_8x4

RIP = Path(__file__).resolve().parent.parent / "rip"
BG_DUMP = RIP / "bg_f003600.bin"
PAL_DUMP = RIP / "pal_0009_f001310.bin"
ROW0, ROWS, COLS = 30, 16, 128

HERO_GRAYS = [(3, 3, 3), (6, 6, 6)]

btiles, _, _ = load_bg_tiles()
btiles = [np.array(t, dtype=np.uint8) for t in btiles]
bpals = np.array(load_palette(PAL_DUMP, 0), dtype=np.uint8)

raw = BG_DUMP.read_bytes()
hist = Counter()
for r in range(ROWS):
    for c in range(COLS):
        idx = scan_8x4(c, ROW0 + r)
        lo, attr = raw[2 * idx], raw[2 * idx + 1]
        code = lo | ((attr & 0x07) << 8)
        color = (attr >> 3) & 0x0F
        t = btiles[code]
        rgb = bpals[color][t]
        cols, cnts = np.unique(rgb.reshape(-1, 3), axis=0, return_counts=True)
        for cc, n in zip(cols, cnts):
            hist[tuple(int(v) for v in cc)] += int(n)

src = np.array(list(hist.keys()), dtype=np.float64)
wgt = np.array(list(hist.values()), dtype=np.float64)
print(f"BG: {len(src)} colori sorgente pesati")

K = 12
cent = src[np.argsort(-wgt)[:K]].copy()
for _ in range(20):
    d = ((src[:, None, :] - cent[None, :, :]) ** 2).sum(2)
    a = d.argmin(1)
    for i in range(K):
        m = a == i
        if m.any():
            cent[i] = (src[m] * wgt[m, None]).sum(0) / wgt[m].sum()

pal = []
for c in cent:
    v = tuple(int(round(x / 255 * 7)) for x in c)
    if v != (0, 0, 0) and v not in pal and v not in HERO_GRAYS:
        pal.append(v)
for i in np.argsort(-wgt):
    if len(pal) >= K:
        break
    v = tuple(int(round(x / 255 * 7)) for x in src[i])
    if v != (0, 0, 0) and v not in pal and v not in HERO_GRAYS:
        pal.append(v)
pal = pal[:K]
pal.sort(key=lambda c: c[0] * 3 + c[1] * 6 + c[2])

pal16 = [(0, 0, 0), (0, 0, 0)] + pal + HERO_GRAYS
print("Palette finale:")
for i, c in enumerate(pal16):
    print(f"  {i:2d}: {c}")
(RIP / "level1_pal.json").write_text(json.dumps(pal16))
print("level1_pal.json scritto")
