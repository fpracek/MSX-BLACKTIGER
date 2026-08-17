#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
MSX2 SCREEN 5 frame encoder with:
- ASxxxx source
- SJASM 0.42 source
- direct 8KB-paged binary
- _SEG099 index page
- duplicate-frame folding
- per-frame bounding rectangle dx,dy,nx,ny in the index page

No Image.Image.getdata() is used.
"""
from __future__ import annotations

import argparse
import shlex
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

import numpy as np
from PIL import Image

PAGE_ORG = 0xA000  # Black Tiger: banked window 0xA000 (bank reg 0xB000)
PAGE_SIZE = 0x2000
VRAM_ROW_BYTES = 128
PAD_BYTE = 0xFF
INDEX_SEG = 99
FIRST_DATA_SEG = 100

SOCCERLG_PALETTE: List[Tuple[int, int, int]] = [
    (0xA3, 0x49, 0xA4),
    (0x01, 0x01, 0x01),
    (0xED, 0x1C, 0x24),
    (0xF7, 0xD6, 0x47),
    (0xFF, 0xFF, 0xFF),
    (0x00, 0x0C, 0x7B),
    (0x00, 0xB8, 0x00),
    (0x7F, 0x7F, 0x7F),
    (0xDD, 0x9C, 0x48),
    (0xF6, 0xD5, 0x43),
    (0x88, 0x00, 0x15),
    (0x4C, 0xB7, 0xDA),
    (0xFA, 0xF7, 0x0F),
    (0xFD, 0xFD, 0xFD),
    (0x3F, 0x48, 0xCC),
    (0xFE, 0xFE, 0xFE),
]
PAL_NP = np.array(SOCCERLG_PALETTE, dtype=np.int16)

# Grauw Z80 instruction table, Timing Z80+M1 column, for emitted instructions.
T = {
    "LD_R_R": 5,
    "LD_R_N": 8,
    "LD_RR_NN": 11,
    "ADD_A_N": 8,
    "ADC_A_N": 8,
    "XOR_N": 8,
    "OR_N": 8,
    "AND_N": 8,
    "INC_R": 5,
    "JP_CC": 11,
    "DI": 5,
    "EI": 5,
    "OUT_N_A": 12,
    "OUTI": 18,
    "RET": 11,
    "LD_A_IYL": 10,
    "LD_IYL_A": 10,
    "LD_I_A": 11,
}

@dataclass
class Run:
    x0: int
    x1: int
    y: int
    data: List[int]

    @property
    def bx(self) -> int:
        return self.x0 // 2

    @property
    def nbytes(self) -> int:
        return len(self.data)

@dataclass
class Bounds:
    dx: int
    dy: int
    nx: int
    ny: int

@dataclass
class Frame:
    index: int
    runs: List[Run]
    offsets: List[int]
    data_bytes: int
    code_bytes: int
    cycles_min: int
    cycles_max: int
    bounds: Bounds
    originals: List[int] = field(default_factory=list)
    page_index: int = -1
    entry_addr: int = 0
    ret_addr: int = 0

    @property
    def empty(self) -> bool:
        return not self.runs

    @property
    def total_bytes(self) -> int:
        return self.data_bytes + self.code_bytes

@dataclass
class Page:
    index: int
    frames: List[Frame] = field(default_factory=list)
    used: int = 0
    binary: bytes = b""
    as_lines: List[str] = field(default_factory=list)
    sj_lines: List[str] = field(default_factory=list)

class BinPage:
    def __init__(self, org: int = PAGE_ORG) -> None:
        self.org = org
        self.buf = bytearray()
        self.labels: Dict[str, int] = {}
        self.fixups: List[Tuple[int, str]] = []

    @property
    def pc(self) -> int:
        return self.org + len(self.buf)

    def size(self) -> int:
        return len(self.buf)

    def label(self, name: str) -> None:
        if name in self.labels:
            raise ValueError(f"Label duplicata: {name}")
        self.labels[name] = self.pc

    def b(self, *vals: int) -> None:
        self.buf.extend(v & 0xFF for v in vals)

    def bs(self, vals: Sequence[int]) -> None:
        self.buf.extend(v & 0xFF for v in vals)

    def wfix(self, label: str) -> None:
        pos = len(self.buf)
        self.buf.extend((0, 0))
        self.fixups.append((pos, label))

    def jpcc(self, opcode: int, label: str) -> None:
        self.b(opcode)
        self.wfix(label)

    def ld_hl_label(self, label: str) -> None:
        self.b(0x21)
        self.wfix(label)

    def resolve(self) -> None:
        for pos, label in self.fixups:
            if label not in self.labels:
                raise ValueError(f"Label non definita: {label}")
            value = self.labels[label]
            self.buf[pos] = value & 0xFF
            self.buf[pos + 1] = (value >> 8) & 0xFF

# ---------------------------------------------------------------------------
# Labels / formatting
# ---------------------------------------------------------------------------

def parse_int(text: str) -> int:
    return int(text, 0)

def frame_label(k: int) -> str:
    return f"Frame{k:04d}"

def data_label(k: int) -> str:
    return f"dataFrame{k:04d}"

def off_label(k: int, n: int) -> str:
    return f"offset{k:04d}_{n:04d}"

def len_label(k: int, n: int) -> str:
    return f"lendatarun{k:04d}_{n:04d}"

def test_label(k: int, n: int) -> str:
    return f"Testrun{k:04d}_{n:04d}"

def pageoff_label(k: int) -> str:
    return f"PageOffset{k:04d}"

def dx_label(k: int) -> str:
    return f"dx{k:04d}"

def dy_label(k: int) -> str:
    return f"dy{k:04d}"

def nx_label(k: int) -> str:
    return f"nx{k:04d}"

def ny_label(k: int) -> str:
    return f"ny{k:04d}"

def as8(v: int) -> str:
    return f"#0x{v & 0xFF:02X}"

def sj8(v: int) -> str:
    return f"0x{v & 0xFF:02X}"

def c_identifier(name: str) -> str:
    out = "".join(ch if (ch.isalnum() or ch == "_") else "_" for ch in name)
    if not out:
        out = "sprite"
    if out[0].isdigit():
        out = "_" + out
    return out

# ---------------------------------------------------------------------------
# Palette / input image
# ---------------------------------------------------------------------------

def extract_palette_from_p(img: Image.Image) -> List[Tuple[int, int, int]]:
    pal = img.getpalette()
    if pal is None or len(pal) < 16 * 3:
        raise ValueError("L'immagine P non contiene una palette valida di almeno 16 colori.")
    return [(int(pal[i * 3]), int(pal[i * 3 + 1]), int(pal[i * 3 + 2])) for i in range(16)]

def quantize_rgb(img: Image.Image) -> np.ndarray:
    arr = np.array(img.convert("RGBA"), dtype=np.uint8)
    rgb = arr[:, :, :3].astype(np.int16)
    alpha = arr[:, :, 3]
    diff = rgb[:, :, None, :] - PAL_NP[None, None, :, :]
    dist2 = np.sum(diff * diff, axis=3)
    idx = np.argmin(dist2, axis=2).astype(np.uint8)
    idx[alpha == 0] = 0
    return idx

def load_indexed_image(path: Path, transparent_arg: Optional[int]) -> Tuple[np.ndarray, List[Tuple[int, int, int]], int, str]:
    img = Image.open(path)
    if img.mode == "P":
        idx = np.array(img, dtype=np.uint8)
        if int(idx.max()) > 15:
            raise ValueError("L'immagine P contiene indici palette > 15. Riduci a 16 colori prima della conversione.")
        pal = extract_palette_from_p(img)
        transparent = 6 if transparent_arg is None else (transparent_arg & 0x0F)
        return idx, pal, transparent, "P"
    idx = quantize_rgb(img)
    return idx, SOCCERLG_PALETTE[:], 0, "RGB/RGBA quantizzata su soccerlg"

def write_palette_h(path: Path, basename: str, pal16: Sequence[Tuple[int, int, int]]) -> None:
    symbol = c_identifier(basename) + "_palette16"
    with path.open("w", encoding="utf-8", newline="\n") as f:
        f.write("// Palette 16xRGB (0..255)\n")
        f.write(f"const unsigned char {symbol}[16][3] = {{\n")
        for r, g, b in pal16:
            f.write(f"    {{{r}, {g}, {b}}},\n")
        f.write("};\n")

# ---------------------------------------------------------------------------
# Frame analysis
# ---------------------------------------------------------------------------

def compute_bounds(frame_idx: np.ndarray, transparent: int) -> Bounds:
    mask = frame_idx != transparent
    ys, xs = np.nonzero(mask)
    if xs.size == 0:
        return Bounds(0, 0, 0, 0)
    x0 = int(xs.min())
    x1 = int(xs.max())
    y0 = int(ys.min())
    y1 = int(ys.max())
    if x0 & 1:
        x0 -= 1
    if not (x1 & 1):
        x1 = min(x1 + 1, frame_idx.shape[1] - 1)
    return Bounds(x0, y0, x1 - x0 + 1, y1 - y0 + 1)

def pack_pixels(row: np.ndarray, x0: int, x1: int) -> List[int]:
    return [((int(row[x]) & 0x0F) << 4) | (int(row[x + 1]) & 0x0F) for x in range(x0, x1 + 1, 2)]

def row_segments(row: np.ndarray, transparent: int) -> List[Tuple[int, int]]:
    mask = row != transparent
    width = int(row.shape[0])
    segs: List[Tuple[int, int]] = []
    x = 0
    while x < width:
        while x < width and not bool(mask[x]):
            x += 1
        if x >= width:
            break
        s = x
        while x + 1 < width and bool(mask[x + 1]):
            x += 1
        e = x
        if s & 1:
            s -= 1
        if not (e & 1):
            e = min(e + 1, width - 1)
        segs.append((s, e))
        x += 1
    if not segs:
        return []
    merged = [segs[0]]
    for s, e in segs[1:]:
        ps, pe = merged[-1]
        if s <= pe + 1:
            merged[-1] = (ps, max(pe, e))
        else:
            merged.append((s, e))
    return merged

def extract_runs(frame_idx: np.ndarray, transparent: int) -> List[Run]:
    runs: List[Run] = []
    for y in range(frame_idx.shape[0]):
        row = frame_idx[y, :]
        for x0, x1 in row_segments(row, transparent):
            runs.append(Run(x0=x0, x1=x1, y=y, data=pack_pixels(row, x0, x1)))
    return runs

def compute_offsets(runs: Sequence[Run]) -> List[int]:
    out: List[int] = []
    prev_abs = 0
    for i, r in enumerate(runs):
        cur_abs = r.bx + VRAM_ROW_BYTES * r.y
        out.append(cur_abs if i == 0 else cur_abs - prev_abs)
        prev_abs = cur_abs
    return out

# ---------------------------------------------------------------------------
# Size / cycle estimates
# ---------------------------------------------------------------------------

def setup_size(off: int, first: bool) -> int:
    if first:
        return 31 if off < 256 else 33
    return 29 if off < 256 else 31

def transfer_size(n: int) -> int:
    return 10 + 2 * n

def code_size(runs: Sequence[Run], offs: Sequence[int]) -> int:
    if not runs:
        return 1
    return 2 + 3 + sum(setup_size(o, i == 0) + transfer_size(r.nbytes) for i, (r, o) in enumerate(zip(runs, offs))) + 1

def transfer_cycles(n: int) -> int:
    return T["LD_R_R"] + T["DI"] + T["OUT_N_A"] + T["LD_R_R"] + T["AND_N"] + T["OUT_N_A"] + T["OUTI"] * n + T["EI"]

def setup_cycles(off: int, first: bool) -> Tuple[int, int]:
    add8 = T["LD_R_R"] + T["ADD_A_N"] + T["LD_R_R"]
    add16 = add8 + T["LD_R_R"] + T["ADC_A_N"] + T["LD_R_R"]
    toggle = T["LD_A_IYL"] + T["XOR_N"] + T["LD_IYL_A"]
    write_first = T["LD_A_IYL"] + T["DI"] + T["OUT_N_A"] + T["LD_R_N"] + T["EI"] + T["OUT_N_A"]
    write_next = T["DI"] + T["OUT_N_A"] + T["LD_R_N"] + T["EI"] + T["OUT_N_A"]
    j = T["JP_CC"]
    if off < 256:
        if first:
            vals = [add8 + j + write_first,
                    add8 + j + T["INC_R"] + j + write_first,
                    add8 + j + T["INC_R"] + j + T["LD_R_N"] + toggle + write_first]
        else:
            vals = [add8 + j,
                    add8 + j + T["INC_R"] + j,
                    add8 + j + T["INC_R"] + j + T["LD_R_N"] + toggle + write_next]
    else:
        if first:
            vals = [add16 + j + write_first,
                    add16 + j + T["LD_R_R"] + T["OR_N"] + T["LD_R_R"] + toggle + write_first]
        else:
            vals = [add16 + j,
                    add16 + j + T["LD_R_R"] + T["OR_N"] + T["LD_R_R"] + toggle + write_next]
    return min(vals), max(vals)

def frame_cycles(runs: Sequence[Run], offs: Sequence[int]) -> Tuple[int, int]:
    if not runs:
        return T["RET"], T["RET"]
    mn = mx = T["LD_R_N"] + T["LD_RR_NN"]
    for i, (r, off) in enumerate(zip(runs, offs)):
        a, b = setup_cycles(off, i == 0)
        tr = transfer_cycles(r.nbytes)
        mn += a + tr
        mx += b + tr
    return mn + T["RET"], mx + T["RET"]

# ---------------------------------------------------------------------------
# Emitters
# ---------------------------------------------------------------------------

def emit_setup_text(lines: List[str], k: int, n: int, off: int, first: bool, asm: str) -> None:
    imm = "#" if asm == "as" else ""
    port = "(#0x99)" if asm == "as" else "(0x99)"
    tlabel = test_label(k, n)
    olabel = off_label(k, n)
    if off < 256:
        lines += ["\tld a,e", f"\tadd a,{imm}{olabel}", "\tld e,a", f"\tjp nc,{tlabel}", "\tinc d", f"\tjp nz,{tlabel}", f"\tld d,{imm}0xC0"]
    else:
        lines += ["\tld a,e", f"\tadd a,{imm}({olabel} & 0xFF)", "\tld e,a", "\tld a,d", f"\tadc a,{imm}({olabel} / 256)", "\tld d,a", f"\tjp nc,{tlabel}", "\tld a,d", f"\tor {imm}0xC0", "\tld d,a"]
    lines += ["\tld a,iyl", f"\txor {imm}1", "\tld iyl,a"]
    if first:
        lines += [f"{tlabel}:", "\tld a,iyl", "\tdi", f"\tout {port},a", f"\tld a,{imm}0x8E", "\tei", f"\tout {port},a"]
    else:
        lines += ["\tdi", f"\tout {port},a", f"\tld a,{imm}0x8E", "\tei", f"\tout {port},a", f"{tlabel}:"]

def emit_setup_bin(bp: BinPage, k: int, n: int, off: int, first: bool) -> None:
    tlabel = test_label(k, n)
    if off < 256:
        bp.b(0x7B, 0xC6, off & 0xFF, 0x5F)
        bp.jpcc(0xD2, tlabel)
        bp.b(0x14)
        bp.jpcc(0xC2, tlabel)
        bp.b(0x16, 0xC0)
    else:
        bp.b(0x7B, 0xC6, off & 0xFF, 0x5F, 0x7A, 0xCE, (off >> 8) & 0xFF, 0x57)
        bp.jpcc(0xD2, tlabel)
        bp.b(0x7A, 0xF6, 0xC0, 0x57)
    bp.b(0xFD, 0x7D, 0xEE, 0x01, 0xFD, 0x6F)
    if first:
        bp.label(tlabel)
        bp.b(0xFD, 0x7D, 0xF3, 0xD3, 0x99, 0x3E, 0x8E, 0xFB, 0xD3, 0x99)
    else:
        bp.b(0xF3, 0xD3, 0x99, 0x3E, 0x8E, 0xFB, 0xD3, 0x99)
        bp.label(tlabel)

def emit_transfer_text(lines: List[str], k: int, n: int, asm: str) -> None:
    port = "(#0x99)" if asm == "as" else "(0x99)"
    if asm == "as":
        lines += ["\tld a,e", "\tdi", f"\tout {port},a", "\tld a,d", "\tand #0x7F", f"\tout {port},a", f"\t.rept #{len_label(k, n)}", "\touti", "\t.endm", "\tei"]
    else:
        lines += ["\tld a,e", "\tdi", f"\tout {port},a", "\tld a,d", "\tand 0x7F", f"\tout {port},a", f"\tREPEAT {len_label(k, n)}", "\t\touti", "\tENDREPEAT", "\tei"]

def emit_transfer_bin(bp: BinPage, nbytes: int) -> None:
    bp.b(0x7B, 0xF3, 0xD3, 0x99, 0x7A, 0xE6, 0x7F, 0xD3, 0x99)
    bp.bs([0xED, 0xA3] * nbytes)
    bp.b(0xFB)

def emit_data_text(lines: List[str], fr: Frame, asm: str) -> None:
    lines.append(f"{data_label(fr.index)}:")
    for n, (r, off) in enumerate(zip(fr.runs, fr.offsets)):
        if asm == "as":
            lines.append(f"{off_label(fr.index, n)}\t\t.equ   {off}")
            lines.append(f"{len_label(fr.index, n)}\t\t.equ   {r.nbytes}")
            lines.append("\t.db " + ",".join(as8(b) for b in r.data) + f"\t\t; dati della run{fr.index:04d}_{n:04d}")
        else:
            lines.append(f"{off_label(fr.index, n)}\tEQU\t{off}")
            lines.append(f"{len_label(fr.index, n)}\tEQU\t{r.nbytes}")
            lines.append("\tdb " + ",".join(sj8(b) for b in r.data) + f"\t\t; dati della run{fr.index:04d}_{n:04d}")

def frame_text_lines(fr: Frame, asm: str) -> List[str]:
    if fr.empty:
        return [f"{frame_label(fr.index)}:\tret"]
    lines: List[str] = []
    emit_data_text(lines, fr, asm)
    lines.append("")
    if asm == "as":
        lines += [f"{frame_label(fr.index)}:", "\tld c,#0x98", f"\tld hl,#{data_label(fr.index)}"]
    else:
        lines += [f"{frame_label(fr.index)}:", "\tld c,0x98", f"\tld hl,{data_label(fr.index)}"]
    for n, (r, off) in enumerate(zip(fr.runs, fr.offsets)):
        emit_setup_text(lines, fr.index, n, off, n == 0, asm)
        emit_transfer_text(lines, fr.index, n, asm)
    lines.append("\tret")
    return lines

def emit_frame_bin(bp: BinPage, fr: Frame) -> None:
    if fr.empty:
        bp.label(frame_label(fr.index))
        fr.entry_addr = bp.pc
        bp.b(0xC9)
        fr.ret_addr = bp.pc - 1
        return
    bp.label(data_label(fr.index))
    for r in fr.runs:
        bp.bs(r.data)
    bp.label(frame_label(fr.index))
    fr.entry_addr = bp.pc
    bp.b(0x0E, 0x98)
    bp.ld_hl_label(data_label(fr.index))
    for n, (r, off) in enumerate(zip(fr.runs, fr.offsets)):
        emit_setup_bin(bp, fr.index, n, off, n == 0)
        emit_transfer_bin(bp, r.nbytes)
    bp.b(0xC9)
    fr.ret_addr = bp.pc - 1

# ---------------------------------------------------------------------------
# Deduplication / packing / pages
# ---------------------------------------------------------------------------

def analyze_and_deduplicate(idx: np.ndarray, N: int, M: int, transparent: int) -> Tuple[List[Frame], List[Frame]]:
    unique_by_signature: Dict[bytes, Frame] = {}
    unique_frames: List[Frame] = []
    original_to_unique: List[Frame] = []
    k = 0
    for fy in range(idx.shape[0] // N):
        for fx in range(idx.shape[1] // M):
            frame_arr = idx[fy * N:(fy + 1) * N, fx * M:(fx + 1) * M]
            signature = frame_arr.tobytes()
            if signature in unique_by_signature:
                fr = unique_by_signature[signature]
                fr.originals.append(k)
                original_to_unique.append(fr)
            else:
                runs = extract_runs(frame_arr, transparent)
                offs = compute_offsets(runs)
                data_b = sum(r.nbytes for r in runs)
                code_b = code_size(runs, offs)
                cmin, cmax = frame_cycles(runs, offs)
                bounds = compute_bounds(frame_arr, transparent)
                fr = Frame(k, runs, offs, data_b, code_b, cmin, cmax, bounds, originals=[k])
                unique_by_signature[signature] = fr
                unique_frames.append(fr)
                original_to_unique.append(fr)
            k += 1
    return unique_frames, original_to_unique

def pack_pages(unique_frames: List[Frame]) -> List[Page]:
    pages: List[Page] = []
    for fr in sorted(unique_frames, key=lambda f: f.total_bytes, reverse=True):
        if fr.total_bytes > PAGE_SIZE:
            raise ValueError(f"Frame{fr.index:04d} supera da sola 8KB: {fr.total_bytes} byte")
        placed = False
        for p in pages:
            if p.used + fr.total_bytes <= PAGE_SIZE:
                p.frames.append(fr)
                p.used += fr.total_bytes
                placed = True
                break
        if not placed:
            pages.append(Page(len(pages), [fr], fr.total_bytes))
    for p in pages:
        p.frames.sort(key=lambda f: f.index)
        for fr in p.frames:
            fr.page_index = p.index
    return pages

def build_data_page(page: Page) -> None:
    bp = BinPage(PAGE_ORG)
    as_lines = [f".area _SEG{FIRST_DATA_SEG + page.index:03d} (ABS)", "\t.org 0xA000", ""]
    sj_lines = ["\tALIGN 0x2000", "\tORG 0xA000", ""]
    for fr in page.frames:
        dup = "" if len(fr.originals) == 1 else f" ; routine condivisa da frame {','.join(f'{x:04d}' for x in fr.originals)}"
        cmt = f"; Frame {fr.index:04d}: bytes={fr.total_bytes} code={fr.code_bytes} data={fr.data_bytes} cycles={fr.cycles_min}..{fr.cycles_max} rect=({fr.bounds.dx},{fr.bounds.dy},{fr.bounds.nx},{fr.bounds.ny}){dup}"
        as_lines += ["; ------------------------------------------------------------", cmt, "; ------------------------------------------------------------"]
        sj_lines += ["; ------------------------------------------------------------", cmt, "; ------------------------------------------------------------"]
        as_lines += frame_text_lines(fr, "as") + [""]
        sj_lines += frame_text_lines(fr, "sj") + [""]
        emit_frame_bin(bp, fr)
    bp.resolve()
    page.used = bp.size()
    if page.used > PAGE_SIZE:
        raise OverflowError(f"_SEG{FIRST_DATA_SEG + page.index:03d} supera 8KB: {page.used}")
    page.binary = bytes(bp.buf) + bytes([PAD_BYTE]) * (PAGE_SIZE - page.used)
    page.as_lines = as_lines
    page.sj_lines = sj_lines

def byte_rect_value(v: int) -> int:
    # Index stores rectangle dimensions as bytes. A value 256 is encoded as 0.
    return v & 0xFF

def build_index_page(original_to_unique: Sequence[Frame]) -> Tuple[bytes, List[str], List[str]]:
    entry_size = 8  # dw frame,page + db dx,dy,nx,ny
    if len(original_to_unique) * entry_size > PAGE_SIZE:
        raise ValueError(f"Pagina indice: {len(original_to_unique)} frame richiedono {len(original_to_unique) * entry_size} byte, massimo {PAGE_SIZE}.")
    as_lines = [f".area _SEG{INDEX_SEG:03d} (ABS)", "\t.org 0xA000", ""]
    sj_lines = ["\tORG 0xA000", ""]
    buf = bytearray()
    for k, canon in enumerate(original_to_unique):
        b = canon.bounds
        as_lines.append(f"{pageoff_label(k)}\t\t.equ   {canon.page_index}")
        as_lines.append(f"{dx_label(k)}\t\t.equ   {b.dx}")
        as_lines.append(f"{dy_label(k)}\t\t.equ   {b.dy}")
        as_lines.append(f"{nx_label(k)}\t\t.equ   {b.nx}")
        as_lines.append(f"{ny_label(k)}\t\t.equ   {b.ny}")
        as_lines.append(f"\t.dw {frame_label(canon.index)},{pageoff_label(k)}")
        as_lines.append(f"\t.db {dx_label(k)},{dy_label(k)},{nx_label(k)},{ny_label(k)}")
        sj_lines.append(f"{pageoff_label(k)}\tEQU\t{canon.page_index}")
        sj_lines.append(f"{dx_label(k)}\tEQU\t{b.dx}")
        sj_lines.append(f"{dy_label(k)}\tEQU\t{b.dy}")
        sj_lines.append(f"{nx_label(k)}\tEQU\t{b.nx}")
        sj_lines.append(f"{ny_label(k)}\tEQU\t{b.ny}")
        sj_lines.append(f"\tdw {frame_label(canon.index)},{pageoff_label(k)}")
        sj_lines.append(f"\tdb {dx_label(k)},{dy_label(k)},{nx_label(k)},{ny_label(k)}")
        buf.extend((canon.entry_addr & 0xFF, (canon.entry_addr >> 8) & 0xFF,
                    canon.page_index & 0xFF, (canon.page_index >> 8) & 0xFF,
                    byte_rect_value(b.dx), byte_rect_value(b.dy), byte_rect_value(b.nx), byte_rect_value(b.ny)))
    binary = bytes(buf) + bytes([PAD_BYTE]) * (PAGE_SIZE - len(buf))
    return binary, as_lines, sj_lines

# ---------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------

def header_lines(fmt: str, cmd: str, N: int, M: int, used_w: int, used_h: int, unique_frames: Sequence[Frame], original_to_unique: Sequence[Frame], pages: Sequence[Page]) -> List[str]:
    useful = PAGE_SIZE + sum(p.used for p in pages)
    padded = PAGE_SIZE * (1 + len(pages))
    lower_bound = (sum(fr.total_bytes for fr in unique_frames) + PAGE_SIZE - 1) // PAGE_SIZE
    lines = [
        "; ============================================================",
        f"; MSX2 SCREEN 5 frame encoder indexed/deduplicated/bounds - {fmt}",
        f"; Command line: {cmd}",
        "; Origin pagina mapper: 0xA000 (Black Tiger)",
        "; Nota indice: nx=256 e' codificato nel byte come 0.",
        f"; Frame: {M}x{N} pixel",
        f"; Area utile immagine: {used_w}x{used_h}",
        f"; Frame totali input: {len(original_to_unique)}",
        f"; Routine frame uniche: {len(unique_frames)}",
        f"; Frame duplicate eliminate: {len(original_to_unique) - len(unique_frames)}",
        f"; Pagine dati: {len(pages)} + indice _SEG099",
        f"; Lower bound teorico pagine dati: {lower_bound}",
        f"; Totale byte utili stimati incluso indice: {useful}",
        f"; Totale byte paginato: {padded}",
        ";",
        "; Tabella frame input: idx | mapping | page | addr | bytes | code | data | cycles | dx dy nx ny",
    ]
    for k, canon in enumerate(original_to_unique):
        mapping = f"Frame{k:04d}" if k == canon.index else f"Frame{k:04d} -> Frame{canon.index:04d}"
        b = canon.bounds
        lines.append(f"; {k:04d} | {mapping} | _SEG{FIRST_DATA_SEG + canon.page_index:03d} | 0x{canon.entry_addr:04X} | {canon.total_bytes:5d} | {canon.code_bytes:5d} | {canon.data_bytes:5d} | {canon.cycles_min:6d}..{canon.cycles_max:6d} | {b.dx:3d} {b.dy:3d} {b.nx:3d} {b.ny:3d}")
    lines += [";", "; Tabella pagine mapper:"]
    lines.append(f"; _SEG099: {len(original_to_unique) * 8} / {PAGE_SIZE} byte indice")
    for p in pages:
        lines.append(f"; _SEG{FIRST_DATA_SEG + p.index:03d}: {p.used} / {PAGE_SIZE} byte")
    lines += ["; ============================================================", ""]
    return lines

def write_outputs(outdir: Path, basename: str, N: int, M: int, used_w: int, used_h: int, cmd: str, pal16: Sequence[Tuple[int, int, int]], unique_frames: List[Frame], original_to_unique: List[Frame], pages: List[Page]) -> None:
    outdir.mkdir(parents=True, exist_ok=True)
    for p in pages:
        build_data_page(p)
    index_bin, index_as, index_sj = build_index_page(original_to_unique)

    pal_path = outdir / f"{basename}_pal.h"
    as_path = outdir / f"Sprite{N}x{M}.asm"
    sj_path = outdir / f"Sprites{N}x{M}_sjasm.asm"
    bin_path = outdir / f"Sprites{N}x{M}.bin"

    write_palette_h(pal_path, basename, pal16)
    as_lines = header_lines("ASxxxx", cmd, N, M, used_w, used_h, unique_frames, original_to_unique, pages) + index_as + [""]
    sj_lines = header_lines("SJASM", cmd, N, M, used_w, used_h, unique_frames, original_to_unique, pages) + index_sj + [""]
    for p in pages:
        as_lines += p.as_lines + [""]
        sj_lines += p.sj_lines + [""]
    as_path.write_text("\n".join(as_lines) + "\n", encoding="utf-8", newline="\n")
    sj_path.write_text("\n".join(sj_lines) + "\n", encoding="utf-8", newline="\n")
    with bin_path.open("wb") as f:
        f.write(index_bin)
        for p in pages:
            f.write(p.binary)

    print(f"Palette C           : {pal_path}")
    print(f"ASM ASxxxx          : {as_path}")
    print(f"ASM SJASM           : {sj_path}")
    print(f"BIN                 : {bin_path}")

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> None:
    ap = argparse.ArgumentParser(description="MSX2 SCREEN 5 indexed/deduplicated/bounds frame encoder. No Image.Image.getdata().")
    ap.add_argument("image", help="Immagine input WxH")
    ap.add_argument("--N", type=int, required=True, help="Altezza frame in pixel")
    ap.add_argument("--M", type=int, required=True, help="Larghezza frame in pixel, deve essere pari")
    ap.add_argument("--transparent", type=parse_int, default=None, help="Indice trasparente. Default: P=6, RGB/RGBA=0 forzato")
    ap.add_argument("-o", "--out", default="output", help="Cartella output")
    args = ap.parse_args()

    if args.N <= 0 or args.M <= 0:
        raise ValueError("N e M devono essere > 0")
    if args.M & 1:
        raise ValueError("M deve essere pari")
    if args.M > 256:
        raise ValueError("M > 256 non supportato: SCREEN 5 ha righe da 256 pixel / 128 byte")

    path = Path(args.image)
    if not path.exists():
        raise FileNotFoundError(path)

    idx, pal16, transparent, kind = load_indexed_image(path, args.transparent)
    h, w = idx.shape
    used_w = (w // args.M) * args.M
    used_h = (h // args.N) * args.N
    if used_w <= 0 or used_h <= 0:
        raise ValueError("Area utile nulla: controlla dimensioni immagine, N e M")
    idx = idx[:used_h, :used_w]

    unique_frames, original_to_unique = analyze_and_deduplicate(idx, args.N, args.M, transparent)
    pages = pack_pages(unique_frames)
    cmd = " ".join(shlex.quote(x) for x in sys.argv)
    write_outputs(Path(args.out), path.stem, args.N, args.M, used_w, used_h, cmd, pal16, unique_frames, original_to_unique, pages)

    lower_bound = (sum(fr.total_bytes for fr in unique_frames) + PAGE_SIZE - 1) // PAGE_SIZE
    print(f"Tipo input          : {kind}")
    print(f"Dimensioni input    : {w}x{h}")
    print(f"Area utile          : {used_w}x{used_h}")
    print(f"Colore trasparente  : {transparent}")
    print(f"Frame input         : {len(original_to_unique)}")
    print(f"Frame uniche        : {len(unique_frames)}")
    print(f"Frame duplicate     : {len(original_to_unique) - len(unique_frames)}")
    print(f"Pagine dati         : {len(pages)}")
    print(f"Lower bound pagine  : {lower_bound}")
    print(f"Pagine totali BIN   : {1 + len(pages)}")
    print("Segmenti mapper:")
    print(f"  _SEG099: {len(original_to_unique) * 8} / {PAGE_SIZE} byte indice")
    for p in pages:
        print(f"  _SEG{FIRST_DATA_SEG + p.index:03d}: {p.used} / {PAGE_SIZE} byte")
    print("Packing             : ottimo rispetto al lower bound teorico" if len(pages) == lower_bound else "Packing             : first-fit decreasing; vicino al minimo teorico")

if __name__ == "__main__":
    main()
