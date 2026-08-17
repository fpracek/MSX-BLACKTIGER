# Black Tiger - round 1 COMPLETO (136x64 celle, 2176x1024 px) dalla TSR
# round1.png. Sostituisce la finestra 120x16 di make_level.py.
#
# Uscite:
#   bin/mapfull.bin   celle u16 LE (136*64*2 = 17408B) -> ROM, letta a strip
#   bin/tilesrom.bin  pixel dei tile 4bpp (N*128B, pad a segmento) -> ROM,
#                     copiati nella cache VRAM on-demand (LRU)
#   bin/level1.bin    solid bitmap (17B/riga * 64) + climb bitmap -> RAM
#   level1_data.h     palette, N_TILES, MAP_W/H, g_TileFlat[N_TILES]
#
# La TSR NON usa le coordinate della dump MAME (vecchia finestra = TSR
# riga 46, colonna 6). Il mondo ora e' la TSR intera: tutte le Y di gioco
# della vecchia finestra vanno spostate di +736 (46*16), le X di +96 (6*16).
import json
import numpy as np
from PIL import Image
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
GFX = TOOLS.parent / "gfx"
RIP = TOOLS.parent / "rip"
PROJ = Path("E:/Dropbox/FAUSTO/SVILUPPI/MSX/CODE/C/MSXgl/projects/blacktiger")

ROWS, COLS = 64, 136
SEG = 8192

tsr = np.array(Image.open(TOOLS.parent / "pixelpump" / "tsr" / "round1.png").convert("RGB"), dtype=np.int16)
pal16 = [tuple(c) for c in json.loads((RIP / "level1_pal.json").read_text())]
pal16_np = np.array([(r * 255 // 7, g * 255 // 7, b * 255 // 7) for r, g, b in pal16], dtype=np.int16)

# quantizzazione globale (background: solo entry 0..13; 14-15 = grigi eroe)
d = tsr[:, :, None, :].astype(np.int32) - pal16_np[None, None, :14, :].astype(np.int32)
idx = np.argmin((d * d).sum(3), axis=2).astype(np.uint8)

# --- dedup tile ---
uniq = {}
tiles = []
cells = np.zeros((ROWS, COLS), dtype=np.uint16)
for r in range(ROWS):
    for c in range(COLS):
        t = idx[r * 16:(r + 1) * 16, c * 16:(c + 1) * 16]
        k = t.tobytes()
        if k not in uniq:
            uniq[k] = len(tiles)
            tiles.append(t.copy())
        cells[r, c] = uniq[k]
N = len(tiles)
print(f"mappa {COLS}x{ROWS}, tile unici: {N}")
assert N <= 512

# --- solidita': stessa euristica della finestra (crema/tan con texture) ---
def is_solid_rgb(rgb):
    f = rgb.astype(np.float64)
    redness = f[:, :, 0].mean() - f[:, :, 2].mean()
    bright = f.mean()
    texture = f.mean(2).std()
    return redness > 40 and bright > 90 and texture > 20

# classificazione sull'RGB ORIGINALE TSR (la palette quantizzata non ha
# verde e appiattisce i contrasti: il quantizzato sovra/sotto-classifica)
cell_solid = [[1 if is_solid_rgb(tsr[r*16:(r+1)*16, c*16:(c+1)*16]) else 0
               for c in range(COLS)] for r in range(ROWS)]
print(f"celle solide (pre-bridging): {sum(map(sum, cell_solid))}")
bridged = 0
for r in range(ROWS):
    for c in range(1, COLS - 1):
        if not cell_solid[r][c] and cell_solid[r][c - 1] and cell_solid[r][c + 1]:
            cell_solid[r][c] = 1
            bridged += 1
print(f"bridging orizzontale: +{bridged}")

# --- pilastri scalabili: bambu' verde (G dominante) ---
def is_climb_rgb(rgb):
    # bambu' verde: frazione di pixel nettamente verdi nella cella
    f = rgb.astype(np.float64)
    green = (f[:, :, 1] > f[:, :, 0] + 30) & (f[:, :, 1] > f[:, :, 2] + 30) & (f[:, :, 1] > 80)
    return green.mean() > 0.15

cell_climb = [[1 if is_climb_rgb(tsr[r*16:(r+1)*16, c*16:(c+1)*16]) else 0
               for c in range(COLS)] for r in range(ROWS)]
# bridging verticale (i bracci usano tile diversi)
vb = 0
for c in range(COLS):
    marked = [r for r in range(ROWS) if cell_climb[r][c]]
    for a, b in zip(marked, marked[1:]):
        if 1 < b - a <= 3:
            for r in range(a + 1, b):
                if not cell_climb[r][c]:
                    cell_climb[r][c] = 1
                    vb += 1
print(f"celle climb: {sum(map(sum, cell_climb))} (vbridge +{vb})")

# regressione: la vecchia finestra (TSR righe 46-61, col 6-125) deve dare
# collisioni simili a quelle collaudate
old_solid = sum(cell_solid[46 + r][6 + c] for r in range(16) for c in range(120))
old_climb = sum(cell_climb[46 + r][6 + c] for r in range(16) for c in range(120))
print(f"regressione finestra vecchia: solid={old_solid} (atteso ~70-90), climb={old_climb} (atteso ~19)")

# --- preview ---
prev = np.zeros((ROWS * 16, COLS * 16, 3), np.uint8)
for r in range(ROWS):
    for c in range(COLS):
        prev[r * 16:(r + 1) * 16, c * 16:(c + 1) * 16] = pal16_np[tiles[cells[r][c]]]
        if cell_solid[r][c]:
            prev[r * 16, c * 16:(c + 1) * 16] = (255, 0, 0)
        if cell_climb[r][c]:
            prev[r * 16:(r + 1) * 16, c * 16] = (0, 255, 0)
Image.fromarray(prev).save(GFX / "levelfull_preview.png")

# --- mapfull.bin: u16 LE ---
blob = bytearray()
for r in range(ROWS):
    for c in range(COLS):
        v = int(cells[r][c])
        blob += bytes((v & 0xFF, v >> 8))
pad = (-len(blob)) % SEG
blob += bytes(pad)
(PROJ / "bin" / "mapfull.bin").write_bytes(blob)
print(f"mapfull.bin: {len(blob)}B ({len(blob)//SEG} segmenti)")

# --- tilesrom.bin: 128B per tile (64 tile per segmento, mai a cavallo) ---
tb = bytearray()
for t in tiles:
    for y in range(16):
        for x in range(0, 16, 2):
            tb.append((int(t[y, x]) << 4) | int(t[y, x + 1]))
pad = (-len(tb)) % SEG
tb += bytes(pad)
(PROJ / "bin" / "tilesrom.bin").write_bytes(tb)
print(f"tilesrom.bin: {len(tb)}B ({len(tb)//SEG} segmenti)")

# --- level1.bin: bitmap solid + climb (17B per riga) ---
def bitmap(cellgrid):
    out = bytearray()
    for r in range(ROWS):
        for byte in range(17):
            v = 0
            for bit in range(8):
                c = byte * 8 + bit
                if c < COLS and cellgrid[r][c]:
                    v |= 1 << bit
            out.append(v)
    return out

lb = bitmap(cell_solid) + bitmap(cell_climb)
(PROJ / "bin" / "level1.bin").write_bytes(lb)
print(f"level1.bin: {len(lb)}B (solid+climb bitmap)")

# --- header ---
lines = ["// Generated by make_level_full.py - Black Tiger round 1 COMPLETO"]
lines.append(f"#define N_TILES {N}")
lines.append(f"#define MAP_W {COLS}")
lines.append(f"#define MAP_H {ROWS}")
lines.append("#define MAP_ROWB 17\t\t// byte per riga nelle bitmap solid/climb")
lines.append("")
pal_bytes = []
for r, g, b in pal16:
    pal_bytes += [(r << 4) | b, g]
lines.append("const unsigned char g_LevelPal[32] = { " + ", ".join(f"0x{v:02X}" for v in pal_bytes) + " };")
lines.append("")
flat = []
n_flat = 0
for t in tiles:
    v = int(t[0, 0])
    if np.all(t == v):
        flat.append((v << 4) | v)
        n_flat += 1
    else:
        flat.append(0xFF)
flat_cells = sum(1 for r in range(ROWS) for c in range(COLS) if flat[cells[r][c]] != 0xFF)
print(f"tile uniformi: {n_flat}, celle piatte: {flat_cells}/{ROWS*COLS}")
lines.append(f"const unsigned char g_TileFlat[{N}] = {{ " + ", ".join(f"0x{v:02X}" for v in flat) + " };")
(PROJ / "level1_data.h").write_text("\n".join(lines) + "\n", newline="\n")
print("level1_data.h scritto")
