#!/usr/bin/env python3
"""Render ripped Black Tiger bg tilemap dumps + palettes to PNG for verification.

Tile entry (2 bytes, hypothesis from MAME blktiger.cpp):
  byte0 = tile code low
  byte1 = attr: bits0-2 tile code high, bits3-6 color, bit7 flipx
Scan (8x4 screens of 16x16 tiles): index = (col&15) + ((row&15)<<4) + ((col&0x70)<<4) + ((row&0x30)<<7)
Palette entry i: lo = pal[i] (RRRRGGGG), hi = pal[1024+i] (xxxxBBBB)
"""
import sys
from pathlib import Path
import numpy as np
from PIL import Image
from decode_gfx import load_bg_tiles

RIP = Path(__file__).resolve().parent.parent / "rip"
OUT = Path(__file__).resolve().parent.parent / "gfx"

MAPW, MAPH = 128, 64  # tiles (8x4 screens of 16x16 tiles)


def load_palette(path, base=0):
    """Return 16 palettes x 16 colors RGB starting at entry `base`."""
    data = path.read_bytes()
    pals = []
    for p in range(16):
        cols = []
        for c in range(16):
            i = base + p * 16 + c
            lo, hi = data[i], data[1024 + i]
            r = (lo >> 4) & 0xF
            g = lo & 0xF
            b = hi & 0xF
            cols.append((r * 17, g * 17, b * 17))
        pals.append(cols)
    return pals


def scan_8x4(col, row):
    return (col & 0x0F) | ((row & 0x0F) << 4) | ((col & 0x70) << 4) | ((row & 0x30) << 7)


def render_map(bg_path, pal_path, out_path, pal_base=0):
    raw = bg_path.read_bytes()  # 16KB = 8192 entries
    tiles, tw, th = load_bg_tiles()
    tiles = [np.array(t, dtype=np.uint8) for t in tiles]
    pals = load_palette(pal_path, pal_base)

    img = np.zeros((MAPH * 16, MAPW * 16, 3), dtype=np.uint8)
    for row in range(MAPH):
        for col in range(MAPW):
            idx = scan_8x4(col, row)
            lo = raw[2 * idx]
            attr = raw[2 * idx + 1]
            code = lo | ((attr & 0x07) << 8)
            color = (attr >> 3) & 0x0F
            flipx = attr & 0x80
            t = tiles[code]
            if flipx:
                t = t[:, ::-1]
            rgb = np.array(pals[color], dtype=np.uint8)[t]
            img[row * 16:(row + 1) * 16, col * 16:(col + 1) * 16] = rgb
    Image.fromarray(img).save(out_path)
    print(f"{out_path.name}: map {MAPW*16}x{MAPH*16}, pal base {pal_base}")


def render_pal_swatch(pal_path, out_path):
    data = pal_path.read_bytes()
    img = np.zeros((64 * 8, 16 * 8, 3), dtype=np.uint8)
    for i in range(1024):
        lo, hi = data[i], data[1024 + i]
        r = ((lo >> 4) & 0xF) * 17
        g = (lo & 0xF) * 17
        b = (hi & 0xF) * 17
        y, x = divmod(i, 16)
        img[y * 8:(y + 1) * 8, x * 8:(x + 1) * 8] = (r, g, b)
    Image.fromarray(img).save(out_path)
    print(f"{out_path.name}: 1024-entry swatch")


if __name__ == "__main__":
    bg = sys.argv[1] if len(sys.argv) > 1 else "bg_f001500.bin"
    pal = sys.argv[2] if len(sys.argv) > 2 else "pal_0004_f001510.bin"
    base = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0
    render_pal_swatch(RIP / pal, OUT / "rip_palette.png")
    render_map(RIP / bg, RIP / pal, OUT / "rip_map.png", base)
