#!/usr/bin/env python3
"""Extract enemy animation compositions from the sprite RAM log.

Same clustering as the hero, but keeps clusters whose dominant color is NOT 0
(hero armor). Outputs enemy_frames.json + enemy_sheet.png (arcade colors,
labelled by dominant color).
"""
import json
from collections import Counter
from pathlib import Path
import numpy as np
from PIL import Image
from decode_gfx import load_sprites
from render_rip import load_palette
from render_sprites import decode_frame, nframes
from analyze_hero import clusters, normalize

RIP = Path(__file__).resolve().parent.parent / "rip"
GFX = Path(__file__).resolve().parent.parent / "gfx"

tiles, _, _ = load_sprites()
tiles = [np.array(t, dtype=np.uint8) for t in tiles]
pals_np = np.array(load_palette(RIP / "pal_0009_f001310.bin", 512), dtype=np.uint8)

comps = Counter()
for i in range(nframes):
    entries = [e for e in decode_frame(i) if 16 <= e[4] <= 224]
    for g in clusters(entries):
        if len(g) < 2 or len(g) > 8:
            continue
        cols = Counter(e[1] for e in g)
        dom, n = cols.most_common(1)[0]
        if dom == 0 or n * 2 < len(g):
            continue
        comps[(dom, normalize(g))] += 1

frequent = [(k, n) for k, n in comps.most_common() if n >= 5]
print(f"Composizioni nemici frequenti (>=5 frame): {len(frequent)}")

cols_n = 8
rows_n = (len(frequent) + cols_n - 1) // cols_n
sheet = np.zeros((rows_n * 72, cols_n * 72, 3), dtype=np.uint8)
lib = []
for k, ((dom, comp), count) in enumerate(frequent):
    ox, oy = (k % cols_n) * 72 + 4, (k // cols_n) * 72 + 4
    for dx, dy, code, color, flip in comp:
        t = tiles[code]
        if flip:
            t = t[:, ::-1]
        rgb = pals_np[color][t]
        mask = t != 15
        if dy > 52 or dx > 52:
            continue
        h = min(16, 72 * rows_n - (oy + dy))
        w = min(16, 72 * cols_n - (ox + dx))
        region = sheet[oy + dy:oy + dy + h, ox + dx:ox + dx + w]
        region[mask[:h, :w]] = rgb[:h, :w][mask[:h, :w]]
    lib.append({"idx": k, "count": count, "color": dom,
                "pieces": [list(p) for p in comp]})

Image.fromarray(sheet).save(GFX / "enemy_sheet.png")
(RIP / "enemy_frames.json").write_text(json.dumps(lib, indent=1))
for e in lib[:16]:
    print(f"  #{e['idx']}: col={e['color']} pezzi={len(e['pieces'])} freq={e['count']}")
print("enemy_sheet.png + enemy_frames.json scritti")
