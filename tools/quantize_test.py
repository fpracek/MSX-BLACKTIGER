#!/usr/bin/env python3
"""Count unique colors in the ripped level map and preview 16-color V9958 quantization."""
from pathlib import Path
import numpy as np
from PIL import Image

GFX = Path(__file__).resolve().parent.parent / "gfx"

img = Image.open(GFX / "rip_map.png").convert("RGB")
arr = np.array(img)

# ignore never-written black-ish background (the exact fill color of unwritten area)
flat = arr.reshape(-1, 3)
colors, counts = np.unique(flat, axis=0, return_counts=True)
print(f"Colori RGB unici totali nel dump: {len(colors)}")

# quantize to 16 colors (median cut), then snap palette to V9958 3-bit/channel
q = img.quantize(colors=16, method=Image.Quantize.MEDIANCUT)
pal = q.getpalette()[:48]
snapped = []
for i in range(16):
    r, g, b = pal[i*3:i*3+3]
    snapped += [round(r / 255 * 7) * 255 // 7, round(g / 255 * 7) * 255 // 7, round(b / 255 * 7) * 255 // 7]
q.putpalette(snapped + [0] * (768 - 48))
q16 = q.convert("RGB")

# side-by-side comparison crop of an interesting area (dragon skeleton zone)
x0, y0, w, h = 0, 480, 1024, 288
top = arr[y0:y0+h, x0:x0+w]
bot = np.array(q16)[y0:y0+h, x0:x0+w]
comp = np.vstack([top, np.full((8, w, 3), 255, np.uint8), bot])
Image.fromarray(comp).save(GFX / "quant_compare.png")
print("Salvato quant_compare.png (sopra: arcade, sotto: 16 colori V9958)")

pal16 = [tuple(snapped[i*3:i*3+3]) for i in range(16)]
print("Palette 16 (RGB 3-bit scalata):", pal16)
