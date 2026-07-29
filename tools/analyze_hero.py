#!/usr/bin/env python3
"""Extract hero animation frame compositions from the sprite RAM log.

Clusters 16x16 sprite pieces by proximity per frame, tracks the persistent
cluster (the hero), normalizes compositions and dedupes them into an
animation frame library. Outputs hero_frames.json + hero_sheet.png.
"""
import json
from collections import Counter
from pathlib import Path
import numpy as np
from PIL import Image
from decode_gfx import load_sprites
from render_rip import load_palette
from render_sprites import decode_frame, nframes, log

RIP = Path(__file__).resolve().parent.parent / "rip"
GFX = Path(__file__).resolve().parent.parent / "gfx"

tiles, _, _ = load_sprites()
tiles = [np.array(t, dtype=np.uint8) for t in tiles]
pals_np = np.array(load_palette(RIP / "pal_0009_f001310.bin", 512), dtype=np.uint8)


def clusters(entries):
    """Union-find clustering of pieces closer than 24px."""
    n = len(entries)
    parent = list(range(n))

    def find(a):
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a

    for i in range(n):
        xi, yi = entries[i][3], entries[i][4]
        for j in range(i + 1, n):
            xj, yj = entries[j][3], entries[j][4]
            if abs(xi - xj) <= 24 and abs(yi - yj) <= 24:
                parent[find(i)] = find(j)
    groups = {}
    for i in range(n):
        groups.setdefault(find(i), []).append(entries[i])
    return list(groups.values())


def hero_cluster(entries):
    """Most hero-like cluster: >=3 pieces, centroid mid-screen, color 0
    dominant (hero armor uses arcade sprite palette group 0)."""
    best = None
    for g in clusters(entries):
        if len(g) < 3 or len(g) > 12:
            continue
        cx = sum(e[3] for e in g) / len(g)
        cy = sum(e[4] for e in g) / len(g)
        if not (60 <= cx <= 220 and 90 <= cy <= 210):
            continue
        c0 = sum(1 for e in g if e[1] == 0)
        if c0 * 2 < len(g):
            continue
        if best is None or len(g) > len(best):
            best = g
    return best


def normalize(g):
    x0 = min(e[3] for e in g)
    y0 = min(e[4] for e in g)
    return tuple(sorted((e[3] - x0, e[4] - y0, e[0], e[1], e[2]) for e in g))


comps = Counter()
for i in range(nframes):
    entries = [e for e in decode_frame(i) if 16 <= e[4] <= 224]  # drop stale rows
    g = hero_cluster(entries)
    if g:
        comps[normalize(g)] += 1

frequent = [(c, n) for c, n in comps.most_common() if n >= 3]
print(f"Composizioni uniche: {len(comps)}, frequenti (>=3 frame): {len(frequent)}")

# render sheet: one 64x64 cell per composition
cols = 8
rows = (len(frequent) + cols - 1) // cols
sheet = np.zeros((rows * 72, cols * 72, 3), dtype=np.uint8)
lib = []
for k, (comp, count) in enumerate(frequent):
    ox, oy = (k % cols) * 72 + 4, (k // cols) * 72 + 4
    for dx, dy, code, color, flip in comp:
        t = tiles[code]
        if flip:
            t = t[:, ::-1]
        rgb = pals_np[color][t]
        mask = t != 15
        h = min(16, 72 * rows - (oy + dy))
        w = min(16, 72 * cols - (ox + dx))
        if h <= 0 or w <= 0 or dy > 56 or dx > 56:
            continue
        region = sheet[oy + dy:oy + dy + h, ox + dx:ox + dx + w]
        region[mask[:h, :w]] = rgb[:h, :w][mask[:h, :w]]
    lib.append({"count": count, "pieces": [list(p) for p in comp]})

Image.fromarray(sheet).save(GFX / "hero_sheet.png")
(RIP / "hero_frames.json").write_text(json.dumps(lib, indent=1))
print(f"hero_sheet.png ({cols}x{rows} celle) + hero_frames.json scritti")
