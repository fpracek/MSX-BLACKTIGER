#!/usr/bin/env python3
"""Convert ripped Black Tiger level chunk to MSX SCREEN5 C data for the prototype.

Output: level1_data.h in the MSXgl blacktiger project:
  - g_LevelPal[16*2]  V9958 palette (byte0 = R<<4|B, byte1 = G, 3-bit each)
  - g_TileSet[N*128]  16x16 tiles, 2px/byte SCREEN5
  - g_Map[MAPH*MAPW]  tile indices (u8)
"""
from pathlib import Path
import numpy as np
from PIL import Image
from decode_gfx import load_bg_tiles
from render_rip import load_palette, scan_8x4

RIP = Path(__file__).resolve().parent.parent / "rip"
GFX = Path(__file__).resolve().parent.parent / "gfx"
PROJ = Path("E:/Dropbox/FAUSTO/SVILUPPI/MSX/CODE/C/MSXgl/projects/blacktiger")

BG_DUMP = RIP / "bg_f003600.bin"
PAL_DUMP = RIP / "pal_0009_f001310.bin"
ROW0, ROWS = 30, 16   # map window: rows 30..45 (cave band)
COLS = 120            # 120 cols keeps unique tiles <= 215 (see RESERVED_SLOTS)

# cache slots reserved for the 48x48 hero scratch area (rows 11-13, cols 13-15
# of the 16x16 slot grid -> VRAM rect x=208..255, y=944..991 in page 3)
RESERVED_SLOTS = {189, 190, 191, 205, 206, 207, 221, 222, 223}

raw = BG_DUMP.read_bytes()
tiles_idx, _, _ = load_bg_tiles()
tiles_idx = [np.array(t, dtype=np.uint8) for t in tiles_idx]
pals = load_palette(PAL_DUMP, 0)
pals_np = np.array(pals, dtype=np.uint8)  # 16 pal x 16 col x 3

# --- collect map window as (code,color,flipx) per cell ---
cells = []
for r in range(ROWS):
    rowc = []
    for c in range(COLS):
        idx = scan_8x4(c, ROW0 + r)
        lo, attr = raw[2 * idx], raw[2 * idx + 1]
        code = lo | ((attr & 0x07) << 8)
        rowc.append((code, (attr >> 3) & 0x0F, (attr & 0x80) >> 7))
    cells.append(rowc)

# --- unique arcade tiles -> rendered RGB ---
uniq = {}
order = []
for rowc in cells:
    for cell in rowc:
        if cell not in uniq:
            uniq[cell] = len(order)
            order.append(cell)
print(f"Tile unici nel window {COLS}x{ROWS}: {len(order)}")
assert len(order) <= 224 - len(RESERVED_SLOTS), "troppi tile unici per gli slot disponibili"

# assign cache slots skipping the scratch block; g_Map stores SLOT numbers
slots = [s for s in range(224) if s not in RESERVED_SLOTS] + list(range(240, 256))
slot_of = {i: slots[i] for i in range(len(order))}

def render_cell(cell):
    code, color, flipx = cell
    t = tiles_idx[code]
    if flipx:
        t = t[:, ::-1]
    return pals_np[color][t]  # 16x16x3

rgb_tiles = [render_cell(c) for c in order]

# --- palette: joint bg+hero palette computed by make_palette.py ---
import json
pal16 = [tuple(c) for c in json.loads(
    (Path(__file__).resolve().parent.parent / "rip" / "level1_pal.json").read_text())]
pal16_np = np.array([(r * 255 // 7, g * 255 // 7, b * 255 // 7) for r, g, b in pal16], dtype=np.int16)
print("Palette (da level1_pal.json):", pal16)

def to_indices(rgb):
    # background maps only onto entries 0..13 (14,15 = hero armor grays)
    # NOTE: int32 — squaring int16 deltas overflows and corrupts the mapping!
    d = rgb[:, :, None, :].astype(np.int32) - pal16_np[None, None, :14, :].astype(np.int32)
    return np.argmin((d * d).sum(3), axis=2).astype(np.uint8)

msx_tiles = [to_indices(t) for t in rgb_tiles]

# --- solidity: cream/tan textured tiles (platforms, rock columns) ---
# cream = high R, low B; blue stones the opposite; pale backdrop = no texture
def is_solid(rgb):
    f = rgb.astype(np.float64)
    redness = f[:, :, 0].mean() - f[:, :, 2].mean()
    bright = f.mean()
    texture = f.mean(2).std()
    return redness > 40 and bright > 90 and texture > 20

solid_tile = [1 if is_solid(t) else 0 for t in rgb_tiles]
print(f"Tile solidi: {sum(solid_tile)} su {len(order)}")

# --- climbable pillar shafts: arcade tile codes 0x45 / 0x49 (any color/flip)
climb_tile = [1 if c[0] in (0x45, 0x49) else 0 for c in order]
cell_climb = [[climb_tile[uniq[cells[r][c]]] for c in range(COLS)] for r in range(ROWS)]
# vertical bridging: arm-crossing segments use other tile codes but the shaft
# runs through them — fill gaps of up to 2 cells between marked cells
vbridged = 0
for c in range(COLS):
    marked = [r for r in range(ROWS) if cell_climb[r][c]]
    for a, b in zip(marked, marked[1:]):
        if 1 < b - a <= 3:
            for r in range(a + 1, b):
                if not cell_climb[r][c]:
                    cell_climb[r][c] = 1
                    vbridged += 1
print(f"Tile pilastro: {sum(climb_tile)} unici, celle: {sum(map(sum, cell_climb))} (bridging verticale +{vbridged})")

# per-cell solidity + horizontal bridging: platform tops are continuous in
# the arcade even where the tile between two bulbs looks empty
cell_solid = [[solid_tile[uniq[cells[r][c]]] for c in range(COLS)] for r in range(ROWS)]
bridged = 0
for r in range(ROWS):
    for c in range(1, COLS - 1):
        if not cell_solid[r][c] and cell_solid[r][c - 1] and cell_solid[r][c + 1]:
            cell_solid[r][c] = 1
            bridged += 1
print(f"Celle solide dopo bridging: +{bridged}")

# --- CEILING (righe 0-1) dalla TSR round1.png: il dump MAME sopra la riga 32
# e' vuoto (tilemap RAM = solo finestra attiva), ma l'arcade ha il soffitto di
# stalattiti; round1.png usa le stesse coordinate 2176x1024 della mappa.
TSR_MAP = GFX.parent / "pixelpump" / "tsr" / "round1.png"
# ATTENZIONE: la TSR NON usa le coordinate della dump MAME! Allineamento
# misurato per correlazione (0.988): nostra riga r = TSR riga r+46,
# nostra colonna c = TSR colonna c+6.
TSR_DY, TSR_DX = 46, 6
tsr_img = np.array(Image.open(TSR_MAP).convert("RGB"), dtype=np.int16)
content_key = {t.tobytes(): i for i, t in enumerate(msx_tiles)}
ceil_tile = []                 # per riga 0-1: indici tile in msx_tiles
n_new_ceil = 0
for r_ours in (0, 1):
    rr = r_ours + TSR_DY
    rowm = []
    for c in range(COLS):
        cc = c + TSR_DX
        cellrgb = tsr_img[rr * 16:(rr + 1) * 16, cc * 16:(cc + 1) * 16, :]
        ti = to_indices(cellrgb)
        k = ti.tobytes()
        if k not in content_key:
            content_key[k] = len(msx_tiles)
            msx_tiles.append(ti)
            n_new_ceil += 1
        rowm.append(content_key[k])
    ceil_tile.append(rowm)
print(f"Soffitto TSR: {n_new_ceil} tile nuovi (tot {len(msx_tiles)})")
assert len(msx_tiles) <= 224 - len(RESERVED_SLOTS) + 16, "troppi tile col soffitto"
slot_of = {i: slots[i] for i in range(len(msx_tiles))}

# mappa per-cella come indici tile: righe 0-1 = soffitto, resto dal rip
map_tile = [[uniq[cells[r][c]] for c in range(COLS)] for r in range(ROWS)]
map_tile[0] = ceil_tile[0]
map_tile[1] = ceil_tile[1]

# --- preview PNG (solid cells outlined in red) ---
prev = np.zeros((ROWS * 16, COLS * 16, 3), np.uint8)
for r in range(ROWS):
    for c in range(COLS):
        ti = map_tile[r][c]
        prev[r*16:(r+1)*16, c*16:(c+1)*16] = pal16_np[msx_tiles[ti]]
        if cell_solid[r][c]:
            prev[r*16, c*16:(c+1)*16] = (255, 0, 0)
            prev[r*16+1, c*16:(c+1)*16] = (255, 0, 0)
        if cell_climb[r][c]:
            prev[r*16:(r+1)*16, c*16] = (0, 255, 0)
            prev[r*16:(r+1)*16, c*16+1] = (0, 255, 0)
Image.fromarray(prev).save(GFX / "level1_msx_preview.png")
print("Preview: gfx/level1_msx_preview.png (bordo rosso = solido)")

# --- emit raw tileset binary as VRAM image of the cache grid ---
# Page 3 cache: 16 tiles per 256px row, tile at its assigned SLOT (reserved
# scratch slots stay empty). Full 14-row grid = 28672 bytes.
slot_tile = {}
for i in range(len(msx_tiles)):
    slot_tile[slot_of[i]] = msx_tiles[i]
empty = np.zeros((16, 16), np.uint8)
blob = bytearray()
for gr in range(16):
    row_tiles = [slot_tile.get(gr * 16 + i, empty) for i in range(16)]
    for y in range(16):
        for t in row_tiles:
            for x in range(0, 16, 2):
                blob.append((int(t[y, x]) << 4) | int(t[y, x + 1]))
(PROJ / "bin").mkdir(exist_ok=True)
(PROJ / "bin" / "tiles.bin").write_bytes(blob)

# --- emit C header: palette + map only ---
lines = []
lines.append("// Generated by make_level.py - Black Tiger level 1 chunk (SCREEN5)")
lines.append(f"#define LEVEL_TILES {len(msx_tiles)}")
lines.append(f"#define MAP_W {COLS}")
lines.append(f"#define MAP_H {ROWS}")
lines.append("")
lines.append("// V9958 palette: byte0 = R<<4|B, byte1 = G (3-bit)")
pal_bytes = []
for r, g, b in pal16:
    pal_bytes += [(r << 4) | b, g]
lines.append("const unsigned char g_LevelPal[32] = { " + ", ".join(f"0x{v:02X}" for v in pal_bytes) + " };")
lines.append("")
# map/solid/climb -> blob binario su segmento raw (il codice fisso e' 24KB!)
blob2 = bytearray()
for r in range(ROWS):
    blob2 += bytes(slot_of[map_tile[r][c]] for c in range(COLS))
for r in range(ROWS):
    blob2 += bytes(cell_solid[r][c] for c in range(COLS))
for r in range(ROWS):
    blob2 += bytes(cell_climb[r][c] for c in range(COLS))
(PROJ / "bin" / "level1.bin").write_bytes(blob2)
print(f"level1.bin: {len(blob2)} byte (map+solid+climb)")
lines.append("// g_Map/g_Solid/g_Climb: in bin/level1.bin (segmento raw), copiati in RAM al boot")

# flat (uniform) tiles by SLOT: fill byte (idx<<4|idx), 0xFF = not uniform.
# DrawColumn uses HMMV runs for these instead of cache HMMM copies.
flat = [0xFF] * 256
n_flat = 0
for i, t in enumerate(msx_tiles):
    v = int(t[0, 0])
    if np.all(t == v):
        flat[slot_of[i]] = (v << 4) | v
        n_flat += 1
flat_cells = sum(1 for r in range(ROWS) for c in range(COLS)
                 if flat[slot_of[map_tile[r][c]]] != 0xFF)
print(f"Tile uniformi: {n_flat}, celle mappa piatte: {flat_cells}/{ROWS*COLS}")
lines.append("")
lines.append("const unsigned char g_TileFlat[256] = { "
             + ", ".join(f"0x{v:02X}" for v in flat) + " };")
(PROJ / "level1_data.h").write_text("\n".join(lines) + "\n", newline="\n")
print(f"level1_data.h + bin/tiles.bin scritti: {len(order)} tiles, tileset {len(blob)} bytes")
