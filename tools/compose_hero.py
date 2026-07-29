#!/usr/bin/env python3
"""Compose hero meta-frames into a 256px-wide P-mode sheet for SpriteEncoder.

- Filters hero compositions (dominant arcade palette group 8)
- Remaps arcade RGB to the 16-color level palette
  (index 0 = transparent, opaque blacks -> index 1, the reserved pure black)
- Uniform cell grid, outputs hero_msx_sheet.png + hero_cells.json
"""
import json
from pathlib import Path
import numpy as np
from PIL import Image
from decode_gfx import load_sprites
from render_rip import load_palette

RIP = Path(__file__).resolve().parent.parent / "rip"
GFX = Path(__file__).resolve().parent.parent / "gfx"

CELL_W, CELL_H = 48, 48     # even, uniform grid; 5 cells per 256px row

tiles, _, _ = load_sprites()
tiles = [np.array(t, dtype=np.uint8) for t in tiles]
spals = np.array(load_palette(RIP / "pal_0009_f001310.bin", 512), dtype=np.int16)
pal16 = np.array(json.loads((RIP / "level1_pal.json").read_text()), dtype=np.int16)
pal16_rgb = pal16 * 255 // 7

lib = json.loads((RIP / "hero_frames.json").read_text())

# --- filter: dominant color 0 (hero armor palette group) ---
heroes = []
for e in lib:
    cols = [p[3] for p in e["pieces"]]
    if cols.count(0) * 2 >= len(cols):
        heroes.append(e)
print(f"Composizioni eroe (dominante col.0): {len(heroes)} su {len(lib)}")


def render_comp(comp):
    """Render composition -> (rgb array, opaque mask), cropped to bbox."""
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
        cell = spals[color][t]
        rgb[dy:dy + 16, dx:dx + 16][m] = cell[m]
        op[dy:dy + 16, dx:dx + 16] |= m
    return rgb, op


def clean_mask(op):
    """Drop isolated opaque pixels (arcade sparkles -> noise at this scale)."""
    p = np.pad(op, 1)
    n = np.zeros(op.shape, dtype=np.int16)
    for dy in (0, 1, 2):
        for dx in (0, 1, 2):
            if dy == 1 and dx == 1:
                continue
            n += p[dy:dy + op.shape[0], dx:dx + op.shape[1]]
    return op & (n >= 2)


def to_level_indices(rgb, op):
    """Nearest level-palette index among 1..15 for opaque pixels, 0 elsewhere.
    NOTE: int32 — squaring int16 deltas overflows and corrupts the mapping!"""
    d = rgb[:, :, None, :].astype(np.int32) - pal16_rgb[None, None, 1:, :].astype(np.int32)
    idx = np.argmin((d * d).sum(3), axis=2).astype(np.uint8) + 1
    idx[~op] = 0
    return idx


# posterize: cluster all hero opaque pixels to 6 tones (uniform armor areas
# instead of per-pixel quantization noise), then map tones to level palette
rendered = []
allpix = []
for e in heroes:
    rgb, op = render_comp(e["pieces"])
    op = clean_mask(op)
    rendered.append((rgb, op, e))
    allpix.append(rgb[op].reshape(-1, 3))
allpix = np.concatenate(allpix).astype(np.float64)

KT = 6
tones = allpix[np.random.default_rng(1).choice(len(allpix), KT, replace=False)]
for _ in range(30):
    d = ((allpix[:, None, :] - tones[None, :, :]) ** 2).sum(2)
    a = d.argmin(1)
    for i in range(KT):
        m = a == i
        if m.any():
            tones[i] = allpix[m].mean(0)
print("Toni eroe:", [tuple(int(x) for x in t) for t in tones])

cells = []
skipped = 0
for rgb, op, e in rendered:
    if rgb.shape[0] > CELL_H or rgb.shape[1] > CELL_W:
        skipped += 1
        continue
    d = ((rgb[:, :, None, :].astype(np.float64) - tones[None, None, :, :]) ** 2).sum(3)
    post = tones[d.argmin(2)].astype(np.int16)
    cells.append((to_level_indices(post, op), e))
print(f"Celle valide {CELL_W}x{CELL_H}: {len(cells)}, saltate (troppo grandi): {skipped}")

# --- TSR walk frames: authentic 6-phase cycles from the Spriters Resource
# barbarian sheet (armored ids 37-42, bare ids 43-48 of tsr_boxes.json) ---
TSR_IMG = np.array(Image.open(GFX.parent / "pixelpump" / "tsr" / "barbarian.png").convert("RGBA"))
TSR_BOXES = json.loads((RIP / "tsr_boxes.json").read_text())

def tsr_cell(idx):
    x0, y0, x1, y1, _ = TSR_BOXES[idx]
    reg = TSR_IMG[y0:y1, x0:x1]
    op = (reg[:, :, 3] > 0) & ~((reg[:, :, 0] == 255) & (reg[:, :, 1] == 0) & (reg[:, :, 2] == 220))
    rgb = reg[:, :, :3].astype(np.int16)
    # TSR art faces LEFT; our base (right-facing) set needs the flip
    return np.fliplr(to_level_indices(rgb, op))

def atk_hit_cell(fig_id, ball_id=9):
    """Strike pose + spiked mace ball at the fist (the ball is a separate
    sprite in the arcade). Fixed 48-wide canvas: figure at x=7 (matching the
    old centered placement so the body doesn't step back), ball overlapping
    the fist by 1px on the right — flipped cell puts it on the left."""
    fig = tsr_cell(fig_id)
    ball = tsr_cell(ball_id)
    fh, fw = fig.shape
    bh, bw = ball.shape
    cell = np.zeros((fh + 6, CELL_W), dtype=np.uint8)
    cell[6:, 7:7 + fw] = fig
    rx = np.nonzero(fig.any(0))[0].max()          # fist = rightmost columns
    fy = int(np.nonzero(fig[:, rx - 1:rx + 1].any(1))[0].mean()) + 6
    bx = 7 + fw - 1
    by = max(0, fy - bh // 2)
    dst = cell[by:by + bh, bx:bx + bw]
    m = ball[:dst.shape[0], :dst.shape[1]] != 0
    dst[m] = ball[:dst.shape[0], :dst.shape[1]][m]
    return cell

def with_ball(fig):
    """Hang the mace ball from the REAR HAND, tracked per frame: the fist is
    the backmost opaque point in the mid-body band, so the ball follows the
    arm swing through the walk cycle. Painted only under the silhouette."""
    ball = tsr_cell(9)
    bh, bw = ball.shape
    fh, fw = fig.shape
    PAD = 6
    cell = np.zeros((fh, fw + PAD), dtype=np.uint8)
    cell[:, PAD:] = fig
    b0, b1 = (fh * 9) // 20, (fh * 3) // 4     # hand band: 45%..75% height
    band = fig[b0:b1, :]
    fx = int(np.nonzero(band.any(0))[0].min())          # fist = backmost column
    ys = np.nonzero(band[:, fx:fx + 2].any(1))[0]
    hy = b0 + int(ys.mean())                            # fist height
    by = min(hy + 1, fh - bh)                           # ball hangs below it
    bx = max(0, PAD + fx - bw // 2)
    reg = cell[by:by + bh, bx:bx + bw]
    b = ball[:reg.shape[0], :reg.shape[1]]
    m = (b != 0) & (reg == 0)
    reg[m] = b[m]
    return cell

# layout per set: [idle, walk0..walk5, jump, atk_wind, atk_hit, hurt,
#                  die0, die1, die2, climb0, climb1] = 16 pose
# idle from ROM comps (armored=0, bare=3); everything else from TSR:
# walks 37-42/43-48, attack wind=3/6 hit=4/7, hurt=82/87,
# jump = side arch verified vs MAME (26 armored, 18 bare twin pose);
# the frontal spread frames (57/62) are the pillar-climb pose, NOT the jump

# deaths: armored 84/85/86 (86 = armor shattering, also the armor-loss pose),
#         bare 88/89/90 (90 = skeleton)
# armored idle = MAME comp 8: the authentic idle WITH the ball under the
# shield (bare idles 3/4/5 already carry it); walks get the dangling ball
armored = ([cells[8][0]] + [with_ball(tsr_cell(i)) for i in range(37, 43)]
           + [tsr_cell(26), tsr_cell(3), atk_hit_cell(4), tsr_cell(82),
              tsr_cell(84), tsr_cell(85), tsr_cell(86),
              tsr_cell(55), tsr_cell(56)])
bare    = ([cells[3][0]] + [with_ball(tsr_cell(i)) for i in range(43, 49)]
           + [tsr_cell(18), tsr_cell(6), atk_hit_cell(7), tsr_cell(87),
              tsr_cell(88), tsr_cell(89), tsr_cell(90),
              tsr_cell(60), tsr_cell(61)])
meta_src = cells[0][1]
cells = [(c, meta_src) for c in armored + bare]
print(f"Set organizzati: {len(cells)} pose (11 corazzate + 11 nude, camminate+attacchi TSR)")

# mirrored (left-facing) variants: frames N..2N-1 = fliplr of frames 0..N-1
N_RIGHT = len(cells)
cells += [(np.fliplr(idx), e) for idx, e in cells]
print(f"Totale con specchiati: {len(cells)} frame (LEFT offset = {N_RIGHT})")

per_row = 256 // CELL_W
rows = (len(cells) + per_row - 1) // per_row
sheet = np.zeros((rows * CELL_H, 256), dtype=np.uint8)
meta = []
for k, (idx, e) in enumerate(cells):
    ox = (k % per_row) * CELL_W
    oy = (k // per_row) * CELL_H
    h, w = idx.shape
    # bottom-center anchor: feet aligned to cell bottom
    px = ox + (CELL_W - w) // 2
    py = oy + CELL_H - h
    sheet[py:py + h, px:px + w] = idx
    meta.append({"frame": k, "count": e["count"], "w": int(w), "h": int(h),
                 "pieces": e["pieces"]})

# guaranteed 1px black outline PER CELL (a global pass would leak rim pixels
# across cell borders into neighbouring frames -> stray dots when drawn)
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
print(f"Outline per cella: aggiunti {total_rim} pixel di contorno nero")

img = Image.fromarray(sheet, mode="P")
flat = []
for r, g, b in pal16_rgb:
    flat += [int(r), int(g), int(b)]
img.putpalette(flat + [0] * (768 - len(flat)))
img.save(GFX / "hero_msx_sheet.png")
(RIP / "hero_cells.json").write_text(json.dumps(meta, indent=1))

# C table of real painted boxes per frame: {xoff, yoff, w, h} within the cell.
# MUST be measured AFTER the rim pass — the 1px outline falls outside the
# content bbox, and an erase box that misses it leaves black trails on screen.
PROJ = Path("E:/Dropbox/FAUSTO/SVILUPPI/MSX/CODE/C/MSXgl/projects/blacktiger")
rows_c = []
for k in range(len(cells)):
    ox = (k % per_row) * CELL_W
    oy = (k // per_row) * CELL_H
    ys, xs = np.nonzero(sheet[oy:oy + CELL_H, ox:ox + CELL_W])
    xo, yo = int(xs.min()), int(ys.min())
    w_, h_ = int(xs.max()) - xo + 1, int(ys.max()) - yo + 1
    rows_c.append(f"\t{{{xo},{yo},{w_},{h_}}},")
hdr = ["// Generated by compose_hero.py - painted box per frame {xoff,yoff,w,h}",
       f"const unsigned char g_HeroBox[{len(cells)}][4] = {{"] + rows_c + ["};"]
(PROJ / "hero_meta.h").write_text("\n".join(hdr) + "\n", newline="\n")
print(f"hero_meta.h: {len(cells)} box")
print(f"hero_msx_sheet.png: {rows} righe x {per_row} celle | hero_cells.json scritto")
