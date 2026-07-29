#!/usr/bin/env python3
"""Decode logged Black Tiger sprite RAM frames and render them to PNGs.

Window logged: 0xF800-0xFFFF per frame. Sprite table hypothesis: 0xFE00
(offset 0x600), 128 entries x 4 bytes [code, attr, y, x]:
  code  = b0 | (attr & 0xE0) << 3
  color = attr & 0x0F
  x     = b3 | (attr & 0x10) << 4
Sprite palette hypothesis: base 0x200, pen 15 transparent.
"""
import sys
from pathlib import Path
import numpy as np
from PIL import Image
from decode_gfx import load_sprites
from render_rip import load_palette

RIP = Path(__file__).resolve().parent.parent / "rip"
GFX = Path(__file__).resolve().parent.parent / "gfx"

WINDOW = 0x800
SPR_OFF = 0x600
PAL_FILE = "pal_0009_f001310.bin"
PAL_BASE = 512          # sprite palette entries start at 0x200

log = (RIP / "sprites_log.bin").read_bytes()
nframes = len(log) // WINDOW
scroll = {}
for line in (RIP / "sprites_scroll.csv").read_text().splitlines():
    f, sx, sy = line.split(",")
    scroll[int(f)] = (int(sx), int(sy))
frames_list = sorted(scroll.keys())

tiles, _, _ = load_sprites()
tiles = [np.array(t, dtype=np.uint8) for t in tiles]
pals = load_palette(RIP / PAL_FILE, PAL_BASE)
pals_np = np.array(pals, dtype=np.uint8)


def decode_frame(i):
    """Entries: (code, color, flipx, x, y). attr = b1:
    bits 7-5 code high, bit 4 x MSB, bit 3 flipx, bits 2-0 color."""
    base = i * WINDOW + SPR_OFF
    out = []
    for s in range(128):
        b0, b1, b2, b3 = log[base + s * 4: base + s * 4 + 4]
        code = b0 | ((b1 & 0xE0) << 3)
        color = b1 & 0x07
        flip = (b1 >> 3) & 1
        x = b3 | ((b1 & 0x10) << 4)
        y = b2
        if code == 0 and b1 == 0 and y == 0:
            continue
        out.append((code, color, flip, x, y))
    return out


def render_frame(i):
    canvas = np.zeros((256, 512, 3), dtype=np.uint8)
    sprites = decode_frame(i)
    for code, color, flip, x, y in sprites:
        t = tiles[code]
        if flip:
            t = t[:, ::-1]
        rgb = pals_np[color][t]
        mask = t != 15
        h = min(16, 256 - y)
        w = min(16, 512 - x)
        if h <= 0 or w <= 0:
            continue
        region = canvas[y:y + h, x:x + w]
        region[mask[:h, :w]] = rgb[:h, :w][mask[:h, :w]]
    return canvas, sprites


if __name__ == "__main__":
    picks = [int(a) for a in sys.argv[1:]] or [200, 800, 1400, 2000, 2600, 3200]
    for p in picks:
        img, sprites = render_frame(p)
        fr = frames_list[p]
        Image.fromarray(img).save(GFX / f"spr_f{fr:05d}.png")
        codes = sorted(set(c for c, _, _, _, _ in sprites))
        print(f"frame {fr}: {len(sprites)} sprites, codes: {codes[:20]}{'...' if len(codes)>20 else ''}")
