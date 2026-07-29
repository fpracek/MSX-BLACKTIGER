#!/usr/bin/env python3
"""Compose the level-1 orc walker into a 256px-wide P-mode sheet.

Filters enemy compositions (color 5, >=6 pieces = the big armored orc),
same treatment as the hero: posterize, level-palette map, per-cell black
outline, mirrored variants. Outputs orc_msx_sheet.png + orc_cells.json.
"""
import json
from pathlib import Path
import numpy as np
from PIL import Image
from decode_gfx import load_sprites
from render_rip import load_palette

RIP = Path(__file__).resolve().parent.parent / "rip"
GFX = Path(__file__).resolve().parent.parent / "gfx"

CELL_W, CELL_H = 48, 48

tiles, _, _ = load_sprites()
tiles = [np.array(t, dtype=np.uint8) for t in tiles]
spals = np.array(load_palette(RIP / "pal_0009_f001310.bin", 512), dtype=np.int16)
pal16 = np.array(json.loads((RIP / "level1_pal.json").read_text()), dtype=np.int16)
pal16_rgb = pal16 * 255 // 7

lib = json.loads((RIP / "enemy_frames.json").read_text())
# the UPRIGHT walking orc: tall comps (the wide 64x32 beast is another creature)
def bbox_h(e):
    return max(p[1] for p in e["pieces"]) + 16
orcs = [e for e in lib if e["color"] == 5 and bbox_h(e) > 32]
print(f"Composizioni orco: {len(orcs)}")


def render_comp(comp):
    xs = [p[0] for p in comp]
    ys = [p[1] for p in comp]
    w, h = max(xs) + 16, max(ys) + 16
    rgb = np.zeros((h, w, 3), dtype=np.int16)
    op = np.zeros((h, w), dtype=bool)
    for dx, dy, code, color, flip in comp:
        t = tiles[code]
        if flip:
            t = t[:, ::-1]
        m = t != 15
        rgb[dy:dy + 16, dx:dx + 16][m] = spals[color][t][m]
        op[dy:dy + 16, dx:dx + 16] |= m
    return rgb, op


def clean_mask(op):
    p = np.pad(op, 1)
    n = np.zeros(op.shape, dtype=np.int16)
    for dy in (0, 1, 2):
        for dx in (0, 1, 2):
            if dy == 1 and dx == 1:
                continue
            n += p[dy:dy + op.shape[0], dx:dx + op.shape[1]]
    return op & (n >= 2)


def to_level_indices(rgb, op):
    d = rgb[:, :, None, :].astype(np.int32) - pal16_rgb[None, None, 1:, :].astype(np.int32)
    idx = np.argmin((d * d).sum(3), axis=2).astype(np.uint8) + 1
    idx[~op] = 0
    return idx


rendered = []
allpix = []
for e in orcs:
    rgb, op = render_comp(e["pieces"])
    op = clean_mask(op)
    rendered.append((rgb, op, e))
    allpix.append(rgb[op].reshape(-1, 3))
allpix = np.concatenate(allpix).astype(np.float64)

KT = 6
tones = allpix[np.random.default_rng(2).choice(len(allpix), KT, replace=False)]
for _ in range(30):
    d = ((allpix[:, None, :] - tones[None, :, :]) ** 2).sum(2)
    a = d.argmin(1)
    for i in range(KT):
        m = a == i
        if m.any():
            tones[i] = allpix[m].mean(0)
print("Toni orco:", [tuple(int(x) for x in t) for t in tones])

cells = []
for rgb, op, e in rendered:
    if rgb.shape[0] > CELL_H or rgb.shape[1] > CELL_W:
        continue
    d = ((rgb[:, :, None, :].astype(np.float64) - tones[None, None, :, :]) ** 2).sum(3)
    post = tones[d.argmin(2)].astype(np.int16)
    cells.append((to_level_indices(post, op), e))
print(f"Celle valide: {len(cells)}")

# --- TSR strike pose: id 13 of the enemies sheet (low lunge, axe flying
# forward). The MAME walk comps already carry the axe overhead = wind-up.
TSR_IMG = np.array(Image.open(GFX.parent / "pixelpump" / "tsr" / "enemies_bosses.png").convert("RGBA"))
TSR_BOXES = json.loads((RIP / "tsr_enemy_boxes.json").read_text())

def tsr_cell(idx):
    x0, y0, x1, y1, _ = TSR_BOXES[idx]
    reg = TSR_IMG[y0:y1, x0:x1]
    op = (reg[:, :, 3] > 0) & ~((reg[:, :, 0] == 255) & (reg[:, :, 1] == 0) & (reg[:, :, 2] == 220))
    # posterize with the SAME tones as the MAME walk frames (color coherence),
    # no flip: the enemies sheet faces right natively (unlike the barbarian)
    rgb = reg[:, :, :3].astype(np.float64)
    d = ((rgb[:, :, None, :] - tones[None, None, :, :]) ** 2).sum(3)
    post = tones[d.argmin(2)].astype(np.int16)
    return to_level_indices(post, op)

cells.append((tsr_cell(13), cells[0][1]))
print(f"Aggiunto affondo TSR (id 13): {len(cells)} pose")

N_RIGHT = len(cells)
cells += [(np.fliplr(idx), e) for idx, e in cells]
print(f"Totale con specchiati: {len(cells)} (LEFT offset = {N_RIGHT})")

per_row = 256 // CELL_W
rows = (len(cells) + per_row - 1) // per_row
sheet = np.zeros((rows * CELL_H, 256), dtype=np.uint8)
meta = []
for k, (idx, e) in enumerate(cells):
    ox = (k % per_row) * CELL_W
    oy = (k // per_row) * CELL_H
    h, w = idx.shape
    px = ox + (CELL_W - w) // 2
    py = oy + CELL_H - h
    sheet[py:py + h, px:px + w] = idx
    meta.append({"frame": k, "count": e["count"], "w": int(w), "h": int(h)})

# per-cell outline
total_rim = 0
for k in range(len(cells)):
    ox = (k % per_row) * CELL_W
    oy = (k // per_row) * CELL_H
    region = sheet[oy:oy + CELL_H, ox:ox + CELL_W]
    op = region != 0
    p = np.pad(op, 1)
    neigh = p[:-2, 1:-1] | p[2:, 1:-1] | p[1:-1, :-2] | p[1:-1, 2:]
    rim = neigh & ~op
    region[rim] = 1
    total_rim += int(rim.sum())
print(f"Outline: {total_rim} px")

img = Image.fromarray(sheet, mode="P")
flat = []
for r, g, b in (pal16 * 255 // 7):
    flat += [int(r), int(g), int(b)]
img.putpalette(flat + [0] * (768 - len(flat)))
img.save(GFX / "orc_msx_sheet.png")
(RIP / "orc_cells.json").write_text(json.dumps(meta, indent=1))

# C table of real painted boxes per frame: {xoff, yoff, w, h} within the cell.
# Measured AFTER the rim pass — the 1px outline falls outside the content bbox.
PROJ = Path("E:/Dropbox/FAUSTO/SVILUPPI/MSX/CODE/C/MSXgl/projects/blacktiger")
rows_c = []
for k in range(len(cells)):
    ox = (k % per_row) * CELL_W
    oy = (k // per_row) * CELL_H
    ys, xs = np.nonzero(sheet[oy:oy + CELL_H, ox:ox + CELL_W])
    xo, yo = int(xs.min()), int(ys.min())
    w_, h_ = int(xs.max()) - xo + 1, int(ys.max()) - yo + 1
    rows_c.append(f"\t{{{xo},{yo},{w_},{h_}}},")
hdr = ["// Generated by compose_enemies.py - painted box per frame {xoff,yoff,w,h}",
       f"const unsigned char g_OrcBox[{len(cells)}][4] = {{"] + rows_c + ["};"]
(PROJ / "orc_meta.h").write_text("\n".join(hdr) + "\n", newline="\n")
print(f"orc_meta.h: {len(cells)} box")
print(f"orc_msx_sheet.png: {rows} righe x {per_row}")
