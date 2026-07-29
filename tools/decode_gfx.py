#!/usr/bin/env python3
"""Decode Black Tiger (Capcom, 1987) graphics ROMs to PNG sheets.

Layouts from MAME blktiger.cpp:
  chars   8x8  4bpp: planes { half+4, half+0, 4, 0 }
  tiles  16x16 4bpp: same plane scheme, Capcom 16x16 layout
  sprites 16x16 4bpp: same as tiles
"""
import sys
from pathlib import Path
from PIL import Image

ROMS = Path(__file__).resolve().parent.parent / "roms"
OUT = Path(__file__).resolve().parent.parent / "gfx"
OUT.mkdir(exist_ok=True)

# grayscale-ish palette for inspection (index 0 = dark blue to spot transparency)
PAL = [(0, 0, 64)] + [(i * 17, i * 17, i * 17) for i in range(1, 16)]


def get_bit(data, bitofs):
    return (data[bitofs >> 3] >> (7 - (bitofs & 7))) & 1


def decode(data, width, height, planes, xofs, yofs, charsize_bits, n):
    """Generic MAME-style gfx decoder. Returns list of tiles (list of rows)."""
    # planes with RGN_FRAC(1,2): half offsets are in bits already resolved by caller
    tiles = []
    for c in range(n):
        base = c * charsize_bits
        tile = []
        for y in range(height):
            row = []
            for x in range(width):
                pix = 0
                for pnum, pofs in enumerate(planes):
                    if get_bit(data, pofs + base + yofs[y] + xofs[x]):
                        pix |= 1 << (len(planes) - 1 - pnum)
                row.append(pix)
            tile.append(row)
        tiles.append(tile)
    return tiles


def decode_region(data, is8x8):
    half_bits = (len(data) * 8) // 2
    planes = [half_bits + 4, half_bits + 0, 4, 0]
    if is8x8:
        xofs = [0, 1, 2, 3, 8, 9, 10, 11]
        yofs = [i * 16 for i in range(8)]
        charsize = 16 * 8
        w = h = 8
    else:
        xofs = [0, 1, 2, 3, 8, 9, 10, 11,
                32 * 8 + 0, 32 * 8 + 1, 32 * 8 + 2, 32 * 8 + 3,
                33 * 8 + 0, 33 * 8 + 1, 33 * 8 + 2, 33 * 8 + 3]
        yofs = [i * 16 for i in range(16)]
        charsize = 64 * 8
        w = h = 16
    # only first half is addressed (second half holds the other 2 planes)
    n = half_bits // charsize
    tiles = decode(data, w, h, planes, xofs, yofs, charsize, n)
    return tiles, w, h


def save_sheet(tiles, w, h, per_row, path):
    rows = (len(tiles) + per_row - 1) // per_row
    img = Image.new("RGB", (per_row * w, rows * h))
    px = img.load()
    for i, tile in enumerate(tiles):
        ox, oy = (i % per_row) * w, (i // per_row) * h
        for y in range(h):
            for x in range(w):
                px[ox + x, oy + y] = PAL[tile[y][x]]
    img.save(path)
    print(f"{path.name}: {len(tiles)} tiles {w}x{h} -> {img.size[0]}x{img.size[1]}")


def load(*names):
    return b"".join((ROMS / n).read_bytes() for n in names)


def load_chars():
    return decode_region(load("bd-15.2n"), is8x8=True)


def load_bg_tiles():
    return decode_region(load("bd-12.5b", "bd-11.4b", "bd-14.9b", "bd-13.8b"), is8x8=False)


def load_sprites():
    return decode_region(load("bd-08.5a", "bd-07.4a", "bd-10.9a", "bd-09.8a"), is8x8=False)


if __name__ == "__main__":
    tiles, w, h = load_chars()
    save_sheet(tiles, w, h, 32, OUT / "chars.png")
    tiles, w, h = load_bg_tiles()
    save_sheet(tiles, w, h, 32, OUT / "tiles.png")
    tiles, w, h = load_sprites()
    save_sheet(tiles, w, h, 32, OUT / "sprites.png")
