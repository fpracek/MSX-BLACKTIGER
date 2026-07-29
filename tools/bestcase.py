#!/usr/bin/env python3
"""Best-case SCREEN 5 test: arcade frame vs hand-optimized 16-color reduction.

Playfield only (HUD would get its own palette band via line interrupt).
Outputs gfx/bestcase_compare.png: arcade | auto median-cut | tuned k-means.
"""
from pathlib import Path
import numpy as np
from PIL import Image

GFX = Path(__file__).resolve().parent.parent / "gfx"
SRC = Path("E:/temp/mame/snap/blktigerb1/0001.png")

img = Image.open(SRC).convert("RGB")
W, H = img.size
play = img.crop((0, 32, W, H))          # playfield without HUD
arr = np.array(play, dtype=np.float64)
flat = arr.reshape(-1, 3)

colors, counts = np.unique(flat.astype(np.uint8).reshape(-1, 3), axis=0, return_counts=True)
print(f"Playfield: {len(colors)} colori unici")


def snap3(c):
    return tuple(int(round(x / 255 * 7)) * 255 // 7 for x in c)


def apply_pal(a, pal):
    p = np.array(pal, dtype=np.float64)
    # perceptual weights on the distance
    wv = np.array([2.0, 4.0, 1.0])
    d = ((a[:, :, None, :] - p[None, None, :, :]) ** 2 * wv).sum(3)
    idx = d.argmin(2)
    return np.array(pal, dtype=np.uint8)[idx]


# --- variant A: plain median cut ---
qa = play.quantize(colors=16, method=Image.Quantize.MEDIANCUT)
palA = [snap3(qa.getpalette()[i*3:i*3+3]) for i in range(16)]
outA = apply_pal(arr, palA)

# --- variant B: tuned — weighted k-means, sqrt weights, protected accents ---
cf = colors.astype(np.float64)
wf = np.sqrt(counts.astype(np.float64))          # flatten dominance
# perceptual space for clustering
wv = np.array([2.0, 4.0, 1.0])

k = 16
init = np.argsort(-wf)[:k]
cent = cf[init].copy()
for _ in range(40):
    d = (((cf[:, None, :] - cent[None, :, :]) ** 2) * wv).sum(2)
    a = d.argmin(1)
    for i in range(k):
        m = a == i
        if m.any():
            cent[i] = (cf[m] * wf[m, None]).sum(0) / wf[m].sum()

palB = []
for c in cent:
    v = snap3(c)
    if v not in palB:
        palB.append(v)
# refill with most-used missing colors
for i in np.argsort(-counts):
    if len(palB) >= 16:
        break
    v = snap3(colors[i])
    if v not in palB:
        palB.append(v)
palB = palB[:16]
outB = apply_pal(arr, palB)

# --- compose comparison (2x) ---
def up(a):
    return np.repeat(np.repeat(a.astype(np.uint8), 2, 0), 2, 1)

pad = np.full((arr.shape[0] * 2, 8, 3), 24, np.uint8)
comp = np.hstack([up(arr), pad, up(outA), pad, up(outB)])

# palette swatch strip under each variant
sw = np.full((24, comp.shape[1], 3), 24, np.uint8)
def put_swatch(x0, pal):
    for i, c in enumerate(pal):
        sw[2:22, x0 + i * 30: x0 + i * 30 + 28] = c
put_swatch(arr.shape[1] * 2 + 8, palA)
put_swatch(arr.shape[1] * 4 + 16, palB)
comp = np.vstack([comp, sw])

Image.fromarray(comp).save(GFX / "bestcase_compare.png")
print("bestcase_compare.png: arcade | median-cut | tuned")
print("Palette tuned:", [tuple(int(x*7//255) for x in c) for c in palB])
