// ─────────────────────────────────────────────────────────────────────────────
//  Black Tiger MSX2+ - engine prototype - 2026 Fausto Pracek
//  Triple buffer (pages 0-2) + 8-way scroll + column streaming from tile
//  cache in page 3. No objects yet: this milestone measures scroll budget.
//
//  Triple buffer pipeline (see soccerlgMSX2):
//    vedo N, scrivo N+1 (colonne entranti), N+2 in coda
//  Border color = raster gauge: bright while streaming/logic runs.
// ─────────────────────────────────────────────────────────────────────────────
#include "msxgl.h"
#include "blacktiger_rawdef.h"
#include "level1_data.h"

#ifndef BANK3_BASE
#define BANK3_BASE 0xA000
#endif

#define RASTER_GAUGE	0			// 1 = border color shows frame budget (debug)
#define TILE_CACHE_Y	768			// page 3 as 16x16 grid of 16px tiles
#define SCROLL_SPEED	2
#define WX_MAX			(MAP_W * 16 - 256)
#define WY_MAX			(MAP_H * 16 - 176)	// banda di gioco: raster 16-191

// Page-3 tail layout: HUD bar at lines 992-1007 (0x1F000, shown by the split),
// sprite tables packed in lines 1008-1023 (pattern needs 2KB alignment)
#define HUD_VRAM		0x1F000UL	// 16 lines x 128 bytes

// map/solid/climb tables live in a raw segment and are copied to RAM at
// boot: hot paths (CellSolid, streaming) need them without bank switching
// Collisioni: bitmap in RAM (17B/riga). La mappa tile (u16) resta in ROM
// (mapfull.bin) e viene letta a strip; i pixel dei tile stanno in ROM
// (tilesrom.bin) e passano dalla cache VRAM on-demand (LRU round-robin).
// Cambio banco 3 minimale (1 scrittura): il mapper Yamanooto e' Konami-SCC
// (reg 0xB000); con ENAR spento la OFFR di SET_BANK_SEGMENT e' un no-op e
// lo shadow g_Bank0Segment non lo legge nessuno. Solo per i percorsi CALDI.
#define BANK3(s)	(*(volatile u8*)0xB000 = (u8)(s))

u8 g_SolidBits[MAP_H * MAP_ROWB];
u8 g_TileFlat[N_TILES];			// caricata da level1.bin (era const nel codice)
u8 g_ClimbBits[MAP_H * MAP_ROWB];
u8 g_TileSlot[N_TILES];			// slot cache del tile, 0xFF = non in cache
u16 g_CacheTile[256];			// tile ospitato da ogni slot fisico
u8 g_CacheClock;				// puntatore round-robin sulla lista usabile
u8 g_RowSlot[3][16];			// riga mondo tenuta da ogni slot-riga di pagina
u16 g_RowBase[3];				// r0 committato per pagina (fast path verticale)
u16 g_MapStrip[16], g_MapStrip2[16];

// offset riga precalcolati: la mul x17 di SDCC costava ~200 cicli a sonda
static const u16 kRowB[64] = { 0, 17, 34, 51, 68, 85, 102, 119, 136, 153, 170, 187, 204, 221, 238, 255, 272, 289, 306, 323, 340, 357, 374, 391, 408, 425, 442, 459, 476, 493, 510, 527, 544, 561, 578, 595, 612, 629, 646, 663, 680, 697, 714, 731, 748, 765, 782, 799, 816, 833, 850, 867, 884, 901, 918, 935, 952, 969, 986, 1003, 1020, 1037, 1054, 1071 };


// world scroll position (pixels)
u16 g_WX = 96;				// vista di partenza (mondo completo)
u16 g_WY = 768;				// 32 + 736
// per-page committed position
u16 g_PX[3];
u16 g_PY[3];
u8  g_View = 0;
volatile bool g_VSynch = FALSE;

// Page-flip handshake: the main loop posts the finished page here and spins;
// the ISR applies page + scroll + curtain INSIDE vblank (no mid-frame tearing).
// The ISR only touches VDP ports while a request is posted, and a request is
// only posted while the main loop spins — so ISR and main never interleave
// register writes.
volatile u8 g_FlipReq = 0xFF;	// page to display, 0xFF = nothing pending
u8 g_FlipX, g_FlipY;			// scroll offsets for that page
u8 g_CurPage = 0;				// latched values shown in the game band
u8 g_CurX = 0, g_CurY = 32;

// Split-screen driven by an IM2 interrupt handler — the BIOS is completely
// out of the interrupt path (vector table + trampoline in high RAM). openMSX
// facts (VDP.cc): the FH "flag" is a ~48-CPU-cycle window, unpollable — only
// the IE1 interrupt syncpoint is reliable; the match line is (R#19 - R#23).
// Phase protocol keeps ISR and main from ever touching the VDP together:
//   main parks (busy=0) -> vblank INT: latch flip, HUD regs, phase=1
//   -> line INT at raster 15 (R#19=239, R#23=224): game regs, release
//   -> main works (busy=1); ISR skips all VDP writes while busy.
__sfr __at(0x99) g_VdpP1;

volatile u8 g_Phase;
volatile u16 g_VblCnt;				// diag: vblank ISR count
u16 g_LoopCnt;					// diag: main loop count

void Im2Handler() __critical __interrupt
{
	// S#1 first: line interrupt (also acks it)
	g_VdpP1 = 1;
	g_VdpP1 = 0x80 | 15;
	u8 s1 = g_VdpP1;
	g_VdpP1 = 0;
	g_VdpP1 = 0x80 | 15;
	if (s1 & 1)
	{
		if (g_Phase == 1)
		{
			g_VdpP1 = (u8)((g_CurPage << 5) | 0x1F);
			g_VdpP1 = 0x80 | 2;
			g_VdpP1 = (u8)((8 - (g_CurX & 7)) & 7);
			g_VdpP1 = 0x80 | 27;
			g_VdpP1 = (u8)(((u16)g_CurX + 7) >> 3);
			g_VdpP1 = 0x80 | 26;
			g_VdpP1 = (u8)(g_CurY - 16);
			g_VdpP1 = 0x80 | 23;
			g_Phase = 0;
		}
	}
	// S#0: v-blank (reading acks it — R#15 is already back on S#0).
	// NOTE: single exit point — an early return would skip the tail EI
	// and leave interrupts dead forever (reti does NOT restore IFF1).
	else if (g_VdpP1 & 0x80)
	{
		// EVERY vblank: bar on, split re-armed. All writes here are plain
		// register pairs on an interrupt-atomic boundary (main's own pairs
		// are DI-wrapped), so no busy gate is needed.
		if (g_FlipReq != 0xFF)
		{
			g_CurPage = g_FlipReq;
			g_CurX = g_FlipX;
			g_CurY = g_FlipY;
			g_FlipReq = 0xFF;
		}
		g_VdpP1 = 0x7F;				// page 3
		g_VdpP1 = 0x80 | 2;
		g_VdpP1 = 0;
		g_VdpP1 = 0x80 | 27;
		g_VdpP1 = 0;
		g_VdpP1 = 0x80 | 26;
		g_VdpP1 = 224;				// page-3 line 224 = VRAM 992
		g_VdpP1 = 0x80 | 23;
		g_Phase = 1;
		g_VblCnt++;
		g_VSynch = TRUE;
	}
	// __critical epilogue is plain reti (no ei): re-arm by hand. EI's
	// one-instruction delay makes the pops safe; a still-pending flag
	// just re-enters cleanly and gets acked.
	__asm__("ei");
}

void FlipAndWait(u8 w)
{
	g_FlipX = (u8)(g_PX[w] & 255);
	g_FlipY = (u8)(g_PY[w] & 255);
	g_FlipReq = w;
	while (g_FlipReq != 0xFF) {}	// released as soon as vblank latches it
}

//-----------------------------------------------------------------------------
// Interrupt-friendly VDP command issue. MSXgl's SetupR32/R36 hold DI across
// the whole 15-byte indirect stream (~280 cycles > one scanline!): with the
// IM2 split that latency makes the raster-15 game-register write drift past
// the line-16/17 boundary -> the whole game band jitters 1px vertically.
// Our ISR only ever writes balanced pairs on port 0x99 (it never touches
// R#17 nor port 0x9B), so the indirect stream is safe to interrupt: only the
// R#17 pair itself needs atomicity (EI-delay trick). Worst-case DI anywhere
// in the frame now stays well under one scanline.
void CmdFlush32() __naked
{
	__asm
	cw32$:
		ld		a, #2
		di
		out		(0x99), a
		ld		a, #0x8F
		out		(0x99), a
		in		a, (0x99)
		rra							; CE -> carry
		ld		a, #0
		out		(0x99), a
		ld		a, #0x8F
		ei
		out		(0x99), a			; EI-delay: restore pair is atomic
		jr		c, cw32$
		ld		a, #32
		di
		out		(0x99), a
		ld		a, #0x91			; 0x80 | 17
		ei
		out		(0x99), a			; pair atomic, stream interruptible
		ld		hl, #_g_VDP_Command
		ld		c, #0x9B
		.rept 15
		outi
		.endm
		ret
	__endasm;
}

void CmdFlush36() __naked
{
	__asm
	cw36$:
		ld		a, #2
		di
		out		(0x99), a
		ld		a, #0x8F
		out		(0x99), a
		in		a, (0x99)
		rra
		ld		a, #0
		out		(0x99), a
		ld		a, #0x8F
		ei
		out		(0x99), a
		jr		c, cw36$
		ld		a, #36
		di
		out		(0x99), a
		ld		a, #0x91
		ei
		out		(0x99), a
		ld		hl, #_g_VDP_Command + 4
		ld		c, #0x9B
		.rept 11
		outi
		.endm
		ret
	__endasm;
}

void CmdHMMM(u16 sx, u16 sy, u16 dx, u16 dy, u16 nx, u16 ny)
{
	g_VDP_Command.SX = sx;
	g_VDP_Command.SY = sy;
	g_VDP_Command.DX = dx;
	g_VDP_Command.DY = dy;
	g_VDP_Command.NX = nx;
	g_VDP_Command.NY = ny;
	g_VDP_Command.ARG = 0;
	g_VDP_Command.CMD = VDP_CMD_HMMM;
	CmdFlush32();
}

void CmdLMMM(u16 sx, u16 sy, u16 dx, u16 dy, u16 nx, u16 ny, u8 op)
{
	g_VDP_Command.SX = sx;
	g_VDP_Command.SY = sy;
	g_VDP_Command.DX = dx;
	g_VDP_Command.DY = dy;
	g_VDP_Command.NX = nx;
	g_VDP_Command.NY = ny;
	g_VDP_Command.ARG = 0;
	g_VDP_Command.CMD = VDP_CMD_LMMM + op;
	CmdFlush32();
}

void CmdHMMV(u16 dx, u16 dy, u16 nx, u16 ny, u8 col)
{
	g_VDP_Command.DX = dx;
	g_VDP_Command.DY = dy;
	g_VDP_Command.NX = nx;
	g_VDP_Command.NY = ny;
	g_VDP_Command.CLR = col;
	g_VDP_Command.ARG = 0;
	g_VDP_Command.CMD = VDP_CMD_HMMV;
	CmdFlush36();
}

//-----------------------------------------------------------------------------
// Copy tileset from ROM segments to VRAM page 3 (tile cache)
// HUD graphics live in a raw segment (bar 2048B + digit strip 480B): the
// fixed code window is only 24KB (0x4000-0x9FFF) and overflows SILENTLY
// into the bank-3 area — keep bulky const data out of it!
void LoadLevelData()
{
	SET_BANK_SEGMENT(3, LEVEL1_BIN_SEG);
	Mem_Copy((const void*)BANK3_BASE, g_SolidBits, MAP_H * MAP_ROWB);
	Mem_Copy((const void*)(BANK3_BASE + MAP_H * MAP_ROWB), g_ClimbBits, MAP_H * MAP_ROWB);
	Mem_Copy((const void*)(BANK3_BASE + 2 * MAP_H * MAP_ROWB), g_TileFlat, N_TILES);
	SET_BANK_SEGMENT(3, 3);
}

void LoadHud()
{
	SET_BANK_SEGMENT(3, HUD_BIN_SEG);
	VDP_WriteVRAM_128K((u8*)BANK3_BASE, (u16)(HUD_VRAM & 0xFFFF), (u8)(HUD_VRAM >> 16), 2048);
	SET_BANK_SEGMENT(3, 3);
}

//-----------------------------------------------------------------------------
// Draw one map tile via HMMM from cache to a buffer page
// Slot usabili della cache: righe 0-13 (senza il blocco scratch 3x3) +
// riga 15 (VRAM 1008-1023). Gli id 224-239 (riga della barra) sono vietati.
#define N_CACHE_SLOTS 231
u8 g_CacheSlots[N_CACHE_SLOTS];	// generata al boot (via 231B di const dal codice)

void BuildCacheSlots()
{
	u8 n = 0;
	for (u16 s = 0; s < 224; s++)
	{
		u8 gr = (u8)(s >> 4), gc = (u8)(s & 15);
		if (gr >= 11 && gr <= 13 && gc >= 13)
			continue;				// blocco scratch 3x3
		g_CacheSlots[n++] = (u8)s;
	}
	for (u16 s = 240; s < 256; s++)
		g_CacheSlots[n++] = (u8)s;
}

// Porta il tile in cache se manca (fault: 16 righe da 8 byte dalla ROM)
u8 EnsureTile(u16 t)
{
	u8 s = g_TileSlot[t];
	if (s != 0xFF)
		return s;
	s = g_CacheSlots[g_CacheClock];
	if (++g_CacheClock >= N_CACHE_SLOTS) g_CacheClock = 0;
	u16 oldt = g_CacheTile[s];
	if (oldt != 0xFFFF)
		g_TileSlot[oldt] = 0xFF;
	g_CacheTile[s] = t;
	g_TileSlot[t] = s;
	BANK3(TILESROM_BIN_SEG + (u8)(t >> 6));
	const u8* srcp = (const u8*)(BANK3_BASE + (((u16)t & 63) << 7));
	u16 va = ((u16)(s >> 4) << 11) + (((u16)s & 15) << 3);	// offset in pag.3
	for (u8 ln = 0; ln < 16; ln++)
	{
		VDP_WriteVRAM_128K((u8*)srcp, (u16)(0x8000 + va), 1, 8);
		srcp += 8;
		va += 128;
	}
	BANK3(3);
	return s;
}

void DrawTile(u16 tile, u16 dx, u16 dy)
{
	u8 s = EnsureTile(tile);
	CmdHMMM(((u16)s & 15) << 4, TILE_CACHE_Y + (((u16)s >> 4) << 4), dx, dy, 16, 16);
}

// Celle (col, riga-mondo degli slot sy0..sy0+n-1) dalla ROM in dst
void FetchMapCol(u8 w, u16 col, u8 sy0, u8 n, u16* dst)
{
	u16 colOff = (u16)(col << 1);
	u8 seg = 0xFF;
	for (u8 i = 0; i < n; i++)
	{
		u16 off = (u16)(kRowB[g_RowSlot[w][(u8)(sy0 + i) & 15]] << 4) + colOff;	// row*272 = row*17*16
		u8 s2 = MAPFULL_BIN_SEG + (u8)(off >> 13);
		if (s2 != seg) { BANK3(s2); seg = s2; }
		dst[i] = *(const u16*)(BANK3_BASE + (off & 0x1FFF));
	}
	BANK3(3);
}

// Draw 8 rows (r0..r0+7) of a map column on page p at world tile column ctx.
// Buffer slot is (ctx & 15): ctx and ctx+16 share the same slot (seam column).
// Runs of flat (uniform) tiles use a single fast HMMV fill instead of
// per-tile cache copies (63% of the map is flat — big streaming saving).
u16 g_SlotDirty[3];				// slots rewritten by streaming, per page

void DrawColumnPart(u8 p, u16 ctx, u8 r0)
{
	g_SlotDirty[p] |= (u16)1 << (ctx & 15);
	FetchMapCol(p, ctx, r0, 8, g_MapStrip);
	u16 dx = (ctx & 15) << 4;
	u16 dyBase = (u16)p << 8;
	u8 r = r0;
	u8 rEnd = r0 + 8;
	while (r < rEnd)
	{
		u16 t = g_MapStrip[r - r0];
		u8 f = g_TileFlat[t];
		if (f != 0xFF)
		{
			u8 run = 1;
			while ((r + run) < rEnd && g_TileFlat[g_MapStrip[r - r0 + run]] == f)
				run++;
			CmdHMMV(dx, dyBase + ((u16)r << 4), 16, (u16)run << 4, f);
			r += run;
		}
		else
		{
			DrawTile(t, dx, dyBase + ((u16)r << 4));
			r++;
		}
	}
}

void DrawColumn(u8 p, u16 ctx)
{
	DrawColumnPart(p, ctx, 0);
	DrawColumnPart(p, ctx, 8);
}

// The shared seam slot, drawn CORRECTLY for both window edges: page x k..15
// of each tile row comes from the left column c0 (its cols k..15 are the
// window's left sliver), page x 0..k-1 from column c0+16 (the right sliver).
// k is even (2px scroll), so every blit is byte-aligned. This replaces the
// sprite curtain: both edges are pixel-perfect by construction.
u8 g_SeamK[3], g_SeamC0[3];		// committed split state per page

// Scroll orizzontale puro (stesso c0, cambia solo k): l'unica parte della
// cucitura che cambia e' la striscia page-x [kOld..kNew) (o inversa): si
// ridisegna SOLO quella invece delle 32 blit del split completo.
void DrawSeamDelta(u8 w, u8 c0, u8 kOld, u8 kNew)
{
	u8 s = (u8)(c0 & 15);
	u16 dyBase = (u16)w << 8;
	u8 x0, wd;
	u16 srcCol;
	if (kNew > kOld) { x0 = kOld; wd = kNew - kOld; srcCol = (u16)(c0 + 16); }
	else             { x0 = kNew; wd = kOld - kNew; srcCol = c0; }
	u16 dx = ((u16)s << 4) + x0;
	FetchMapCol(w, srcCol, 0, 16, g_MapStrip);
	for (u8 r = 0; r < 16; r++)
	{
		u16 t = g_MapStrip[r];
		u8 f = g_TileFlat[t];
		u16 dy = dyBase + ((u16)r << 4);
		if (f != 0xFF)
			CmdHMMV(dx, dy, wd, 16, f);
		else
		{
			u8 cs = EnsureTile(t);
			CmdHMMM(((((u16)cs & 15) << 4) + x0), TILE_CACHE_Y + (((u16)cs >> 4) << 4), dx, dy, wd, 16);
		}
	}
}

void DrawSeamSplit(u8 w, u8 c0, u8 k)
{
	u8 s = (u8)(c0 & 15);
	u16 dx = (u16)s << 4;
	u16 dyBase = (u16)w << 8;
	FetchMapCol(w, c0, 0, 16, g_MapStrip);
	FetchMapCol(w, (u16)(c0 + 16), 0, 16, g_MapStrip2);
	for (u8 r = 0; r < 16; r++)
	{
		u16 dy = dyBase + ((u16)r << 4);
		u16 t = g_MapStrip2[r];
		u8 f = g_TileFlat[t];
		if (f != 0xFF)
			CmdHMMV(dx, dy, k, 16, f);
		else
		{
			u8 cs = EnsureTile(t);
			CmdHMMM(((u16)cs & 15) << 4, TILE_CACHE_Y + (((u16)cs >> 4) << 4), dx, dy, k, 16);
		}
		t = g_MapStrip[r];
		f = g_TileFlat[t];
		if (f != 0xFF)
			CmdHMMV(dx + k, dy, (u16)(16 - k), 16, f);
		else
		{
			u8 cs = EnsureTile(t);
			CmdHMMM((((u16)cs & 15) << 4) + k, TILE_CACHE_Y + (((u16)cs >> 4) << 4), dx + k, dy, (u16)(16 - k), 16);
		}
	}
}



//-----------------------------------------------------------------------------
// Compiled sprites, ArtRag INDEXED encoder for every object (hero, orc,
// items16, armor32): first segment = index page (8-byte records: entry
// address, data page, restore box dx/dy/nx/ny), data pages bin-packed after
// it. Frame-code contract: DE = VRAM addr (14 bit) | 0xC000, IYl = FULL
// R#14; the routine writes R#14 itself on its first run and RETs.
#define HERO_FRAMES		72
#define HERO_W			48
#define HERO_H			48

u8 g_SprR14, g_SprD, g_SprE;

// x, y in page coords (world & 255). Constraints: x even, x <= 208, y <= 208.
u16 g_SprEntryW;				// absolute entry address from the index

void CallIdxFrame() __naked
{
	__asm
		ld	a, (_g_SprR14)
		.db	#0xFD
		ld	l, a				; iyl = full R#14 (new-encoder contract)
		ld	a, (_g_SprD)
		ld	d, a
		ld	a, (_g_SprE)
		ld	e, a
		ld	hl, (_g_SprEntryW)
		jp	(hl)				; frame code RETs to our caller
	__endasm;
}

// ArtRag indexed encoder (same scheme as the orc): HERO_BIN_SEG = index
// page, data pages follow. 23 segments instead of 38 (FN=2).
void DrawHero(u8 frame, u8 page, u8 x, u8 y)
{
	BANK3(HERO_BIN_SEG);
	const u8* rec = (const u8*)(BANK3_BASE + ((u16)frame << 3));
	g_SprEntryW = (u16)(rec[0] | ((u16)rec[1] << 8));
	u8 pg = rec[2];
	BANK3(HERO_BIN_SEG + 1 + pg);
	g_SprR14 = (page << 1) | (y >> 7);
	g_SprD = (u8)(((y & 0x7F) >> 1) | 0xC0);
	g_SprE = (u8)((y << 7) | (x >> 1));
	CallIdxFrame();
	BANK3(3);
}

// Restore boxes {dx,dy,nx,ny} read ONCE at boot from the encoder INDEX pages
// (single source of truth — hero_meta.h / orc_meta.h are retired). The
// per-frame min-box makes every erase copy only the painted rectangle.
u8 g_HeroBox[HERO_FRAMES][4];
u8 g_OrcBox[6][4];
u8 g_ItBoxIdx[6][4];
u8 g_ArmorBoxIdx[4];

void CopyBoxes(u8 seg, u8* dst, u8 n)
{
	SET_BANK_SEGMENT(3, seg);
	const u8* rec = (const u8*)(BANK3_BASE + 4);
	for (u8 k = 0; k < n; k++)
	{
		dst[0] = rec[0]; dst[1] = rec[1]; dst[2] = rec[2]; dst[3] = rec[3];
		dst += 4;
		rec += 8;
	}
	SET_BANK_SEGMENT(3, 3);
}

void LoadBoxes()
{
	CopyBoxes(HERO_BIN_SEG, &g_HeroBox[0][0], HERO_FRAMES);
	CopyBoxes(ORC_BIN_SEG, &g_OrcBox[0][0], 6);
	CopyBoxes(ITEMS16_BIN_SEG, &g_ItBoxIdx[0][0], 6);
	CopyBoxes(ARMOR32_BIN_SEG, g_ArmorBoxIdx, 1);
}

// Ground truth: the world column currently held by each slot of each page.
// Updated by streaming, read by object erase — they can never disagree.
u8 g_SlotCol[3][16];

// Level-triggered streaming: compare each slot's actual content with what
// the current window requires and rewrite the differences. Self-healing.
// Kept as a SMALL function: inlined in main, SDCC spilled everything to an
// ix frame with shift loops (41% of the frame for 16 compares!).
// Streaming VERTICALE level-triggered: ogni slot-riga di pagina (16, uno
// per riga mondo modulo 16) viene confrontato con le righe richieste dalla
// finestra; le differenze si ridisegnano (16 celle alle colonne correnti di
// g_SlotCol). La pagina e' 256px, la banda 176: 5 righe di slack, quindi
// NIENTE split verticale — le righe entrano fuori schermo.
void DrawRowSeg(u8 w, u16 row, u8 sy)
{
	u16 dy = ((u16)w << 8) + ((u16)sy << 4);
	for (u8 sx = 0; sx < 16; sx++)
	{
		u16 c = g_SlotCol[w][sx];
		if (c >= MAP_W)
			continue;				// sentinella seam: la ricompone lo split
		u16 off = (u16)(kRowB[row] << 4) + (u16)(c << 1);
		BANK3(MAPFULL_BIN_SEG + (u8)(off >> 13));
		u16 t = *(const u16*)(BANK3_BASE + (off & 0x1FFF));
		BANK3(3);
		u8 f = g_TileFlat[t];
		if (f != 0xFF)
			CmdHMMV((u16)sx << 4, dy, 16, 16, f);
		else
			DrawTile(t, (u16)sx << 4, dy);
	}
}

void ReconcileRows(u8 w)
{
	u16 r0 = g_WY >> 4;
	if (g_RowBase[w] == r0)
		return;						// niente scroll verticale per questa pagina
	g_RowBase[w] = r0;
	u8 streamed = 0;
	for (u8 j = 0; j < 12; j++)
	{
		u16 r = r0 + j;
		if (r >= MAP_H) break;
		u8 sy = (u8)(r & 15);
		if (g_RowSlot[w][sy] == r)
			continue;
		g_RowSlot[w][sy] = r;
		DrawRowSeg(w, r, sy);
		streamed = 1;
	}
	if (streamed)
	{
		g_SlotDirty[w] = 0xFFFF;	// ogni skip oggetto va invalidato
		g_SeamK[w] = 0xFF;			// e la cucitura ricomposta
	}
}

void ReconcileSlots(u8 w)
{
	u8* sc = g_SlotCol[w];
	u8 c0 = (u8)(g_WX >> 4);
	u8 k = (u8)(g_WX & 15);
	for (u8 j = 0; j < 16; j++)
	{
		u8 c = c0 + j;
		u8 s = c & 15;
		if (j == 0 && k != 0)
			continue;					// seam slot: DrawSeamSplit owns it
		if (sc[s] == c)
			continue;
		sc[s] = c;
		// full column at once: the deferred-bottom-half trick assumed a
		// 2px/frame camera; with 60Hz sub-steps the window jumps up to
		// 8px per rendered frame and the missing half showed at the edges
		DrawColumnPart(w, c, 0);
		DrawColumnPart(w, c, 8);
	}
}

// Restore the map tiles under an object box committed at page coords px0,py0.
// Flat-tile runs collapse into single HMMV fills (object boxes are mostly
// cave void / backdrop — the big erase cost saver).
void RestoreBox(u8 w, u8 px0, u8 py0, u8 nCols, u8 nRows)
{
	u8 s0 = px0 >> 4;
	u8 ty0 = py0 >> 4;
	u8 tyEnd = ty0 + nRows;
	if (tyEnd > 16) tyEnd = 16;
	for (u8 i = 0; i < nCols; i++)
	{
		u8 slot = (s0 + i) & 15;
		u16 c = g_SlotCol[w][slot];
		if (c >= MAP_W)
			continue;
		FetchMapCol(w, c, ty0, (u8)(tyEnd - ty0), g_MapStrip);
		u16 dx = (u16)slot << 4;
		u8 ty = ty0;
		while (ty < tyEnd)
		{
			u16 t = g_MapStrip[ty - ty0];
			u8 f = g_TileFlat[t];
			if (f != 0xFF)
			{
				u8 run = 1;
				while ((ty + run) < tyEnd && g_TileFlat[g_MapStrip[ty - ty0 + run]] == f)
					run++;
				CmdHMMV(dx, ((u16)w << 8) + ((u16)ty << 4), 16, (u16)run << 4, f);
				ty += run;
			}
			else
			{
				DrawTile(t, dx, ((u16)w << 8) + ((u16)ty << 4));
				ty++;
			}
		}
	}
}

// Erase only the really painted box of a committed frame (exact cols/rows)
void EraseObj(u8 w, u8 px, u8 py, const u8* box)
{
	u8 ex = px + box[0];
	u8 ey = py + box[1];
	u8 nCols = (u8)(((ex & 15) + box[2] + 15) >> 4);
	u8 nRows = (u8)(((ey & 15) + box[3] + 15) >> 4);
	RestoreBox(w, ex, ey, nCols, nRows);
}

// --- controllable hero ---
// Poses (cells of the encoded sheet)
// Pose layout: [armored 0-17][bare 18-35][mirrored 36-71]
// per-set order: idle, walk0-5, jump, atk_wind, atk_hit, hurt, die0-2, climb0-3
// deaths: armored die2 = armor shattering; bare die0-2 = collapse -> skeleton
#define HERO_IDLE		0
#define HERO_WALK		1		// +0..5 phase
#define HERO_JUMP		7
#define HERO_ATK_WIND	8		// mace raised (wind-up / recover)
#define HERO_ATK_HIT	9		// lunge, mace extended
#define HERO_HURT		10
#define HERO_DIE		11		// +0..2 death phase
#define HERO_CLIMB		14		// +0..3 grip phase (frontal, on pillars)
#define HERO_BARE		18		// offset of the unarmored set
#define HERO_LEFT		36		// offset of the mirrored (left-facing) frames
#define ATK_FRAMES		16		// attack action duration
#define ATK_REACH		14		// mace hitbox reach beyond the hero box
#define INVULN_FRAMES	60
#define GROUND_Y		864		// 128 + 736 (offset mondo completo)		// hero top y when standing (feet on row 11)
#define CAM_OFFSET		104		// camera keeps hero ~centered
#define CAM_VOFF		120		// vertical anchor: hero ~2/3 down the screen

// VRAM scratch for seam-crossing draws (reserved 3x3 slot block in the cache)
#define SCRATCH_X		208
#define SCRATCH_Y		944

u16 g_HeroX = 192;				// 96 + 96				// world x (even)
u16 g_HeroY = GROUND_Y;			// world y (top)
i8  g_HeroVY = 0;
u8  g_HeroAir = 0;
u8  g_HeroFacing = 0;			// 0 = right, HERO_LEFT = left
u8  g_HeroFrame = HERO_IDLE;
u8  g_HeroDrawn[3];				// pose committed per page (0xFF = none)
u8  g_HXp[3], g_HYp[3];			// committed page coords per page
u8  g_ScratchFrame = 0xFF;		// pose currently rendered in the scratch
u8  g_HeroAtk = 0;				// attack countdown
u8  g_Invuln = 0;				// post-hit invulnerability (blink)
u8  g_HeroArmor = 1;			// 1 = armored, 0 = bare (one hit from death)
u8  g_HeroStun = 0;				// hit reaction: hurt pose, input frozen
u8  g_HeroClimb = 0;			// clinging to a pillar shaft
u8  g_NoGrab = 0;				// grab suppression after leaping off
u8  g_Coyote = 0;				// grace frames to jump after leaving a ledge
u8  g_GravTick = 0;				// half-rate gravity on descent (long arcs)
u8  g_HeroDead = 0;				// death sequence countdown

// --- orc enemies (patrol AI, compiled sprites 64x32, FN=1) ---
#define ORC_W			48
#define ORC_H			48
#define ORC_LEFT		3		// mirrored frames offset
#define ORC_STRIKE		2		// axe lunge pose (walk pose = axe overhead)
#define ORC_WALK_FRAMES	4
#define N_ORC			2

u16 g_OrcX[N_ORC] = { 432, 768 };	// +96
u16 g_OrcY[N_ORC] = { 880, 880 };	// +736	// platform top (192) - ORC_H
i8  g_OrcDir[N_ORC] = { 1, -1 };
u8  g_OrcAlive[N_ORC] = { 1, 1 };
u8  g_OrcDying[N_ORC];			// death blink countdown
u16 g_OrcResp[N_ORC];			// respawn countdown when dead
const u16 g_OrcSpawnX[N_ORC] = { 432, 768 };
u8  g_OrcFrame[N_ORC];
u8  g_OrcAtk[N_ORC];			// axe attack countdown
u8  g_OrcDrawn[3][N_ORC];
u8  g_OrcXp[3][N_ORC], g_OrcYp[3][N_ORC];
u8  g_OrcTick = 0;
u16 g_OrcPK[N_ORC];				// cache sonda patrol: (probe>>4)|dir, 0xFFFF = invalida
u8  g_OrcMode[N_ORC];			// engagement calcolato al primo substep del frame
u8  g_ItOnlyDyn;				// 0 = primo substep del frame renderizzato
u8  g_OrcBlk[N_ORC];

// ArtRag indexed encoder: ORC_BIN_SEG = index page (8-byte records),
// data pages follow. Any frame id is valid data — no FN contract.
void DrawOrc(u8 frame, u8 page, u8 x, u8 y)
{
	BANK3(ORC_BIN_SEG);
	const u8* rec = (const u8*)(BANK3_BASE + ((u16)frame << 3));
	g_SprEntryW = (u16)(rec[0] | ((u16)rec[1] << 8));
	u8 pg = rec[2];
	BANK3(ORC_BIN_SEG + 1 + pg);
	g_SprR14 = (page << 1) | (y >> 7);
	g_SprD = (u8)(((y & 0x7F) >> 1) | 0xC0);
	g_SprE = (u8)((y << 7) | (x >> 1));
	CallIdxFrame();
	BANK3(3);
}

// --- map collision ---
u8 CellSolid(u16 px, u16 py)		// world pixel coords
{
	if (py >= MAP_H * 16)
		return 1;					// safety floor below the map
	u16 c = px >> 4;
	if (c >= MAP_W)
		return 1;					// side walls
	return (g_SolidBits[kRowB[py >> 4] + (c >> 3)] >> (c & 7)) & 1;
}

u8 CellClimb(u16 px, u16 py)		// climbable pillar shaft at world coords
{
	if (py >= MAP_H * 16)
		return 0;
	u16 c = px >> 4;
	if (c >= MAP_W)
		return 0;
	return (g_ClimbBits[kRowB[py >> 4] + (c >> 3)] >> (c & 7)) & 1;
}

u8 HeroSupported()
{
	u16 fy = g_HeroY + HERO_H;
	return CellSolid(g_HeroX + 16, fy) | CellSolid(g_HeroX + 31, fy);
}

// Render the current pose in the VRAM scratch (only when it changes)
void UpdateScratch(u8 frame)
{
	if (g_ScratchFrame == frame)
		return;
	g_ScratchFrame = frame;
	CmdHMMV(SCRATCH_X, SCRATCH_Y, HERO_W, HERO_H, 0x00);
	VDP_CommandWait();			// the compiled draw must not race the fill
	DrawHero(frame, 3, SCRATCH_X, SCRATCH_Y - 768);
}

// Draw the hero at page coords, splitting across the 256px seam when needed
void DrawHeroAuto(u8 frame, u8 w, u8 px, u8 py)
{
	if (px <= 256 - HERO_W)
	{
		DrawHero(frame, w, px, py);
		return;
	}
	UpdateScratch(frame);
	u8 w1 = (u8)(0 - px);		// = 256 - px
	u16 dy = ((u16)w << 8) + py;
	CmdLMMM(SCRATCH_X, SCRATCH_Y, px, dy, w1, HERO_H, VDP_OP_TIMP);
	CmdLMMM(SCRATCH_X + w1, SCRATCH_Y, 0, dy, HERO_W - w1, HERO_H, VDP_OP_TIMP);
}

// Orc seam-crossing draw: shares the hero's scratch block — the cache key is
// tagged with bit7 so hero poses (0-55) and orc poses never collide. When both
// cross the seam at once the scratch just re-renders per draw (rare, only slow)
void UpdateScratchOrc(u8 frame)
{
	u8 key = 0x80 | frame;
	if (g_ScratchFrame == key)
		return;
	g_ScratchFrame = key;
	CmdHMMV(SCRATCH_X, SCRATCH_Y, ORC_W, ORC_H, 0x00);
	VDP_CommandWait();			// the compiled draw must not race the fill
	DrawOrc(frame, 3, SCRATCH_X, SCRATCH_Y - 768);
}

void DrawOrcAuto(u8 frame, u8 w, u8 px, u8 py)
{
	if (px <= 256 - ORC_W)
	{
		DrawOrc(frame, w, px, py);
		return;
	}
	UpdateScratchOrc(frame);
	u8 w1 = (u8)(0 - px);
	u16 dy = ((u16)w << 8) + py;
	CmdLMMM(SCRATCH_X, SCRATCH_Y, px, dy, w1, ORC_H, VDP_OP_TIMP);
	CmdLMMM(SCRATCH_X + w1, SCRATCH_Y, 0, dy, ORC_W - w1, ORC_H, VDP_OP_TIMP);
}

// Orc partially outside the camera window: draw only the visible slice from
// the scratch (a fully-hidden-or-shown 48px orc pops at the screen edges and
// reads as a teleport). cl = pixels clipped at the left, vw = visible width.
void DrawOrcSlice(u8 frame, u8 w, u8 px, u8 py, u8 cl, u8 vw)
{
	UpdateScratchOrc(frame);
	u16 dy = ((u16)w << 8) + py;
	u8 sx = (u8)(SCRATCH_X + cl);
	u8 dx = (u8)(px + cl);
	u16 w1 = 256 - dx;
	if (vw <= w1)
		CmdLMMM(sx, SCRATCH_Y, dx, dy, vw, ORC_H, VDP_OP_TIMP);
	else
	{
		CmdLMMM(sx, SCRATCH_Y, dx, dy, (u8)w1, ORC_H, VDP_OP_TIMP);
		CmdLMMM((u8)(sx + w1), SCRATCH_Y, 0, dy, (u8)(vw - w1), ORC_H, VDP_OP_TIMP);
	}
}

//-----------------------------------------------------------------------------
// Frame-body helpers. Kept as SMALL functions: SDCC generates ix-frame hell
// (20+cy per local access) inside a big main — see ReconcileSlots lesson.
u8 g_WalkTick;
u8 g_WalkPhase;
u16 g_AtkL, g_AtkR;				// live mace hitbox (0 = inactive)

// camera follows the hero on both axes (same max speed: never falls behind)
void CameraFollow()
{
	u16 target = (g_HeroX > CAM_OFFSET) ? g_HeroX - CAM_OFFSET : 0;
	if (target > WX_MAX) target = WX_MAX;
	if (g_WX < target)      g_WX += 2;
	else if (g_WX > target) g_WX -= 2;
	u16 ty = (g_HeroY + 24 > CAM_VOFF) ? g_HeroY + 24 - CAM_VOFF : 0;
	if (ty > WY_MAX) ty = WY_MAX;
	if (g_WY < ty)      g_WY += 2;
	else if (g_WY > ty) g_WY -= 2;
}

void ThrowDagger();				// defined with the item block below

void HeroLogic()
{
	g_AtkL = 0;
	g_AtkR = 0;
	// death sequence: knocked -> collapsed -> skeleton, blink-out, respawn
	if (g_HeroDead)
	{
		u8 ds = (g_HeroArmor ? 0 : HERO_BARE) + g_HeroFacing;
		if (g_HeroDead > 96)      g_HeroFrame = HERO_HURT + ds;
		else if (g_HeroDead > 72) g_HeroFrame = HERO_DIE + ds;
		else if (g_HeroDead > 48) g_HeroFrame = HERO_DIE + 1 + ds;
		else                      g_HeroFrame = HERO_DIE + 2 + ds;
		if (--g_HeroDead == 0)
		{
			g_HeroX = 192;
			g_HeroY = GROUND_Y;
			g_HeroVY = 0;
			g_HeroAir = 0;
			g_HeroClimb = 0;
			g_HeroArmor = 1;
			g_Invuln = INVULN_FRAMES;
		}
		return;
	}

	// hit stun (armor absorbed the blow): hurt reaction, then the armor
	// flies apart — both drawn with the ARMORED set even if the flag is off
	if (g_HeroStun)
	{
		g_HeroStun--;
		if (g_Invuln) g_Invuln--;
		g_HeroFrame = ((g_HeroStun < 8) ? HERO_DIE + 2 : HERO_HURT) + g_HeroFacing;
		return;
	}

	// input: matrix row 8 — bit4=LEFT, bit7=RIGHT, bit0=SPACE (jump)
	// horizontal wall blocking: probes at body height rise while jumping
	u8 keys = ~Keyboard_Read(8);
	u8 moving = 0;

	// clinging to a pillar: up/down climbs, left/right leaps off
	if (g_HeroClimb)
	{
		u8 gripMove = 0;
		if (keys & 0x20)
		{
			u16 ny = g_HeroY - 2;
			if (CellClimb(g_HeroX + 24, ny + 16)) { g_HeroY = ny; gripMove = 1; }
		}
		else if (keys & 0x40)
		{
			u16 ny = g_HeroY + 2;
			if (CellClimb(g_HeroX + 24, ny + 40)) { g_HeroY = ny; gripMove = 1; }
			else
			{
				// slid off the bottom end: let go
				g_HeroClimb = 0;
				g_HeroAir = 1;
				g_HeroVY = 0;
			}
		}
		if (g_HeroClimb && (keys & 0x90))
		{
			// leap off with direction (pillar jump)
			g_HeroFacing = (keys & 0x80) ? 0 : HERO_LEFT;
			g_HeroClimb = 0;
			g_HeroAir = 1;
			g_HeroVY = -6;
			g_NoGrab = 12;		// don't re-stick to the shaft just left
		}
		if (g_HeroClimb)
		{
			u8 cset = (g_HeroArmor ? 0 : HERO_BARE) + g_HeroFacing;
			g_HeroFrame = HERO_CLIMB + ((g_HeroY >> 2) & 3) + cset;
			(void)gripMove;
			CameraFollow();
			return;
		}
	}
	// airborne long-jump: 4/2 alternating = 3px/frame average (x stays even)
	u8 step = (g_HeroAir && !(g_OrcTick & 1)) ? 4 : 2;
	if (keys & 0x80)
	{
		g_HeroFacing = 0;
		u16 nx = g_HeroX + step;
		if ((nx <= MAP_W * 16 - HERO_W)
		    && !CellSolid(nx + 31, g_HeroY + 24)
		    && !CellSolid(nx + 31, g_HeroY + 44))
		{ g_HeroX = nx; moving = 1; }
	}
	if (keys & 0x10)
	{
		g_HeroFacing = HERO_LEFT;
		if (g_HeroX >= step)
		{
			u16 nx = g_HeroX - step;
			if (!CellSolid(nx + 16, g_HeroY + 24)
			    && !CellSolid(nx + 16, g_HeroY + 44))
			{ g_HeroX = nx; moving = 1; }
		}
	}
	// attack: SPACE starts the mace thrust AND throws a dagger (arcade
	// base weapon: flail swing + thrown dagger together)
	if ((keys & 0x01) && !g_HeroAtk)
	{
		g_HeroAtk = ATK_FRAMES;
		ThrowDagger();
	}
	if (g_HeroAtk)
		g_HeroAtk--;
	if (g_Invuln)
		g_Invuln--;
	// mace hitbox while the thrust is extended (mid-action frames)
	if (g_HeroAtk >= 4 && g_HeroAtk <= 12)
	{
		if (g_HeroFacing == 0) { g_AtkL = g_HeroX + 32; g_AtkR = g_HeroX + HERO_W + ATK_REACH; }
		else if (g_HeroX >= ATK_REACH) { g_AtkL = g_HeroX - ATK_REACH; g_AtkR = g_HeroX + 16; }
	}

	// physics: jump (UP), walk-off-edge fall, land on solid surfaces
	// grabbing a pillar shaft beats jumping (UP/DOWN at the shaft grabs on)
	if (!g_HeroAir)
	{
		if ((keys & 0x60) && CellClimb(g_HeroX + 24, g_HeroY + 24))
		{
			g_HeroX = (u16)(((g_HeroX + 24) & 0xFFF0) - 16);	// centre on shaft
			g_HeroVY = 0;
			g_HeroClimb = 1;
			u8 cset = (g_HeroArmor ? 0 : HERO_BARE) + g_HeroFacing;
			g_HeroFrame = HERO_CLIMB + cset;
			CameraFollow();
			return;
		}
		g_Coyote = 6;
		if (keys & 0x20)             { g_HeroAir = 1; g_HeroVY = -9; }
		else if (!HeroSupported())   { g_HeroAir = 1; g_HeroVY = 0; }
	}
	else if ((keys & 0x20) && g_Coyote && g_HeroVY >= 0)
	{
		// coyote jump: UP pressed a moment after walking off the ledge
		g_Coyote = 0;
		g_HeroVY = -9;
	}
	else if (!g_NoGrab
	         && (CellClimb(g_HeroX + 24, g_HeroY + 8)
	          || CellClimb(g_HeroX + 24, g_HeroY + 20)
	          || CellClimb(g_HeroX + 24, g_HeroY + 30)))
	{
		// airborne contact with a shaft: stick to it (leaping between pillars)
		g_HeroX = (u16)(((g_HeroX + 24) & 0xFFF0) - 16);
		g_HeroAir = 0;
		g_HeroVY = 0;
		g_HeroClimb = 1;
		u8 cset = (g_HeroArmor ? 0 : HERO_BARE) + g_HeroFacing;
		g_HeroFrame = HERO_CLIMB + cset;
		CameraFollow();
		return;
	}
	if (g_NoGrab)
		g_NoGrab--;
	if (g_HeroAir && g_Coyote)
		g_Coyote--;
	if (g_HeroAir)
	{
		u16 oldY = g_HeroY;
		g_HeroY = (u16)((i16)g_HeroY + g_HeroVY);
		// full gravity going up, half-rate coming down: long arcade arcs
		if (g_HeroVY < 0)
			g_HeroVY++;
		else if (g_HeroVY < 5)
		{
			g_GravTick ^= 1;
			if (g_GravTick) g_HeroVY++;
		}
		if ((i16)g_HeroY < 0) { g_HeroY = 0; g_HeroVY = 1; }
		if (g_HeroVY > 0)
		{
			u16 nf = g_HeroY + HERO_H;
			for (u16 by = (u16)((oldY + HERO_H + 15) & 0xFFF0); by <= nf; by += 16)
			{
				if (CellSolid(g_HeroX + 16, by) | CellSolid(g_HeroX + 31, by))
				{
					g_HeroY = by - HERO_H;
					g_HeroAir = 0;
					g_HeroVY = 0;
					break;
				}
			}
		}
	}
	// animation: pose + armor set + facing
	u8 set = (g_HeroArmor ? 0 : HERO_BARE) + g_HeroFacing;
	if (g_HeroAtk)
	{
		// wind-up (raise) -> lunge while the hitbox is live -> recover
		g_HeroFrame = ((g_HeroAtk > 12 || g_HeroAtk < 4)
		               ? HERO_ATK_WIND : HERO_ATK_HIT) + set;
	}
	else if (g_HeroAir)
		g_HeroFrame = HERO_JUMP + set;
	else if (moving)
	{
		// authentic 6-phase walk: advance one phase every 4 frames
		g_WalkTick++;
		if (g_WalkTick >= 4)
		{
			g_WalkTick = 0;
			if (++g_WalkPhase >= 6) g_WalkPhase = 0;
		}
		g_HeroFrame = HERO_WALK + g_WalkPhase + set;
	}
	else
		g_HeroFrame = HERO_IDLE + set;

	CameraFollow();
}



// Snap each orc's feet to the first solid floor under its spawn point
void InitOrcs()
{
	for (u8 e = 0; e < N_ORC; e++)
	{
		u16 fx = g_OrcX[e] + ORC_W / 2;
		for (u8 r = (u8)(g_OrcY[e] >> 4); r < MAP_H; r++)
		{
			if (CellSolid(fx, (u16)r << 4))
			{
				g_OrcY[e] = ((u16)r << 4) - ORC_H;
				break;
			}
		}
	}
}

void SpawnZenny(u16 x, u16 y);	// defined with the item block below

// Shared hit routine: knockback, armor loss (GnG) or death
void HurtHero(u16 ox)
{
	g_Invuln = INVULN_FRAMES;
	if (g_HeroClimb) { g_HeroClimb = 0; g_HeroAir = 1; g_HeroVY = 0; }
	if (g_HeroX > ox) { if (g_HeroX < MAP_W * 16 - HERO_W - 16) g_HeroX += 16; }
	else              { if (g_HeroX >= 16) g_HeroX -= 16; }
	if (g_HeroArmor)
	{
		g_HeroArmor = 0;		// armor absorbs the hit (GnG style)
		g_HeroStun = 16;		// visible hit reaction
	}
	else
	{
		g_HeroDead = 120;
		g_Invuln = 0;
	}
}

void OrcAI()
{
	g_OrcTick++;

	for (u8 e = 0; e < N_ORC; e++)
	{
		if (!g_OrcAlive[e])
		{
			if (g_OrcResp[e]) g_OrcResp[e]--;
			else
			{
				// respawn off-view only
				if (g_OrcSpawnX[e] + ORC_W < g_WX || g_OrcSpawnX[e] > g_WX + 256)
				{
					g_OrcAlive[e] = 1;
					g_OrcX[e] = g_OrcSpawnX[e];
				}
			}
			continue;
		}

		// mace hit? start the death blink
		if (!g_OrcDying[e]
		    && g_AtkR && g_OrcX[e] + ORC_W - 8 > g_AtkL && g_OrcX[e] + 8 < g_AtkR
		    && g_HeroY + HERO_H > g_OrcY[e] && g_OrcY[e] + ORC_H > g_HeroY)
			g_OrcDying[e] = 48;

		// dying: blink in place, then gone
		if (g_OrcDying[e])
		{
			if (--g_OrcDying[e] == 0)
			{
				g_OrcAlive[e] = 0;
				g_OrcResp[e] = 300;		// ~5s
				SpawnZenny(g_OrcX[e] + 16, g_OrcY[e] + ORC_H - 16);
			}
			continue;					// no patrol/contact while dying
		}

		// contact damage: knockback + invulnerability blink
		if (!g_Invuln && !g_HeroDead
		    && g_HeroX + 40 > g_OrcX[e] + 10 && g_OrcX[e] + 38 > g_HeroX + 8
		    && g_HeroY + HERO_H > g_OrcY[e] + 4 && g_OrcY[e] + ORC_H > g_HeroY + 8)
			HurtHero(g_OrcX[e]);

		// engagement: 0 = patrol, 1 = charge, 2 = hold — il calcolo (16 bit,
		// pesante per SDCC) si fa solo al primo substep del frame; negli
		// altri si riusa (deriva max 12px per frame: irrilevante)
		u8 mode;
		if (!g_ItOnlyDyn)
		{
			mode = 0;
			if (!g_HeroDead
			    && g_HeroY + HERO_H >= g_OrcY[e] && g_HeroY <= g_OrcY[e] + ORC_H)
			{
				u16 dxo = (g_HeroX > g_OrcX[e]) ? (g_HeroX - g_OrcX[e]) : (g_OrcX[e] - g_HeroX);
				if (dxo < 80)
				{
					g_OrcDir[e] = (g_HeroX > g_OrcX[e]) ? 1 : -1;
					mode = (dxo > 44) ? 1 : 2;
				}
			}
			g_OrcMode[e] = mode;
		}
		else
			mode = g_OrcMode[e];

		// axe attack: swings while holding at range, axe already overhead
		u8 dirsel = (g_OrcDir[e] < 0) ? ORC_LEFT : 0;
		if (g_OrcAtk[e])
		{
			g_OrcAtk[e]--;
			u8 ph = g_OrcAtk[e];
			if (ph >= 7 && ph <= 16)
			{
				// lunge frame + axe hitbox ahead of the body
				g_OrcFrame[e] = ORC_STRIKE + dirsel;
				if (!g_Invuln && !g_HeroDead)
				{
					// lunge sprite box: full cell width, low (y 19..47)
					u16 axL, axR;
					if (g_OrcDir[e] > 0) { axL = g_OrcX[e] + 24; axR = g_OrcX[e] + ORC_W; }
					else { axL = g_OrcX[e]; axR = g_OrcX[e] + 24; }
					if (g_HeroX + 40 > axL && axR > g_HeroX + 8
					    && g_HeroY + HERO_H > g_OrcY[e] + 19 && g_OrcY[e] + ORC_H > g_HeroY)
						HurtHero(g_OrcX[e]);
				}
			}
			else
				g_OrcFrame[e] = dirsel;		// tension / recover: axe up
			continue;
		}
		if (mode == 2 && !g_HeroDead && ((g_OrcTick + (e << 5)) & 63) == 0)
		{
			g_OrcAtk[e] = 24;
			g_OrcFrame[e] = dirsel;
			continue;
		}

		u8 moved = 0;
		if (mode != 2)
		{
			u16 probe = g_OrcX[e] + ((g_OrcDir[e] > 0) ? ORC_W - 8 : 8);
			// il risultato delle sonde e' stabile finche' la sonda resta
			// nella stessa cella 16px: cache per orco (mappa statica)
			u16 pkey = (probe >> 4) | ((g_OrcDir[e] > 0) ? 0x8000 : 0);
			u8 blocked;
			if (pkey == g_OrcPK[e])
				blocked = g_OrcBlk[e];
			else
			{
				blocked = !CellSolid(probe, g_OrcY[e] + ORC_H) ||
				    CellSolid(probe, g_OrcY[e] + ORC_H - 8) ||
				    (g_OrcDir[e] < 0 && g_OrcX[e] <= 2) ||
				    (g_OrcDir[e] > 0 && g_OrcX[e] >= MAP_W * 16 - ORC_W - 2);
				g_OrcPK[e] = pkey;
				g_OrcBlk[e] = blocked;
			}
			if (blocked)
			{
				if (mode == 0)
					g_OrcDir[e] = -g_OrcDir[e];	// patrol: turn around
				// charge blocked at an edge: hold, keep facing the hero
			}
			else
			{
				g_OrcX[e] += g_OrcDir[e];
				moved = 1;
				if (mode == 1)
				{
					u16 p2 = g_OrcX[e] + ((g_OrcDir[e] > 0) ? ORC_W - 8 : 8);
					if (CellSolid(p2, g_OrcY[e] + ORC_H)
					    && !CellSolid(p2, g_OrcY[e] + ORC_H - 8))
						g_OrcX[e] += g_OrcDir[e];
				}
			}
		}

		// walk anim only while moving; standing keeps a stable pose
		g_OrcFrame[e] = (moved ? ((g_OrcTick >> 3) & 1) : 0) + dirsel;
	}
}

// --- items: breakable vases, zenny drops, armor pickup ---
// gfx cells in the page-3 cache free area (VRAM x64-159, y992-1023):
// cell 0 vase, 1 cracked, 2 shattered, 3 zenny(50), 4 armor (32x32)
#define N_ITEM		12
#define IT_NONE		0
#define IT_VASE		1
#define IT_ZENNY	2
#define IT_ARMOR	3
#define IT_DAGGER	4
#define CELL_ARMOR	0xFE		// draw-dispatch sentinel: 32x32 armor

u16 g_ItX[N_ITEM], g_ItY[N_ITEM];
u8  g_ItType[N_ITEM];
u8  g_ItContent[N_ITEM];		// what a vase reveals
u8  g_ItBreak[N_ITEM];			// vase break animation countdown
u16 g_ItTimer[N_ITEM];			// dropped zenny expiry
u8  g_ItCell[N_ITEM];			// gfx cell to show (0xFF = nothing)
u8  g_ItPop[N_ITEM];			// just revealed: visible but not collectable yet
u8  g_ItDrawn[3][N_ITEM];		// committed cell per page
u8  g_ItXp[3][N_ITEM], g_ItYp[3][N_ITEM];
u16 g_Zenny;					// wallet
u16 g_ZennyShown;				// value currently drawn in the HUD
u8  g_ArmorShown;
u8  g_HudPix[240];				// 12 rows x 20 bytes (5 digits, 4bpp)
u8 BoxOverlap(u8 ax, u8 ay, u8 aw, u8 ah, u8 bx, u8 by, u8 bw, u8 bh);

// scratch arrays for PageObjects (globals: SDCC ix-frame cost)
u8 g_ItPxT[N_ITEM], g_ItVisT[N_ITEM], g_ItRedraw[N_ITEM];

// Items are compiled sprites like hero/orc: Z80 plots, VDP restores.
// items16.bin (seg ITEMS16, FN=4): cells 0 vase, 1 cracked, 2 shattered, 3 zenny
// armor32.bin (seg ARMOR32, FN=1): the 32x32 armor
void DrawItem16(u8 cell, u8 page, u8 x, u8 y)
{
	BANK3(ITEMS16_BIN_SEG);
	const u8* rec = (const u8*)(BANK3_BASE + ((u16)cell << 3));
	g_SprEntryW = (u16)(rec[0] | ((u16)rec[1] << 8));
	u8 pg = rec[2];
	BANK3(ITEMS16_BIN_SEG + 1 + pg);
	g_SprR14 = (page << 1) | (y >> 7);
	g_SprD = (u8)(((y & 0x7F) >> 1) | 0xC0);
	g_SprE = (u8)((y << 7) | (x >> 1));
	CallIdxFrame();
	BANK3(3);
}

void DrawItem32(u8 page, u8 x, u8 y)
{
	BANK3(ARMOR32_BIN_SEG);
	const u8* rec = (const u8*)BANK3_BASE;		// frame 0
	g_SprEntryW = (u16)(rec[0] | ((u16)rec[1] << 8));
	u8 pg = rec[2];
	BANK3(ARMOR32_BIN_SEG + 1 + pg);
	g_SprR14 = (page << 1) | (y >> 7);
	g_SprD = (u8)(((y & 0x7F) >> 1) | 0xC0);
	g_SprE = (u8)((y << 7) | (x >> 1));
	CallIdxFrame();
	BANK3(3);
}

const u16 kVaseX[3] = { 224, 576, 720 };	// +96
const u8  kVaseContent[3] = { IT_ZENNY, IT_ARMOR, IT_ZENNY };

void InitItems()
{
	for (u8 i = 0; i < 3; i++)
	{
		u16 x = kVaseX[i];
		u16 y = 768;				// 32 + 736
		while (y < MAP_H * 16 - 16 && !CellSolid(x + 8, y + 16)) y += 16;
		g_ItType[i] = IT_VASE;
		g_ItContent[i] = kVaseContent[i];
		g_ItX[i] = x;
		g_ItY[i] = y;
	}
}

// Arcade base weapon: every mace swing throws a FAN OF 3 daggers (one
// straight, one drifting up, one drifting down); up to 2 volleys in flight.
void ThrowDagger()
{
	u8 flying = 0;
	for (u8 i = 0; i < N_ITEM; i++)
		if (g_ItType[i] == IT_DAGGER)
			flying++;
	if (flying > 3)
		return;							// max 2 volleys on screen
	u8 vy = 0;							// spawn order: straight, up, down
	for (u8 i = 0; i < N_ITEM && vy < 3; i++)
		if (g_ItType[i] == IT_NONE)
		{
			g_ItType[i] = IT_DAGGER;
			g_ItContent[i] = g_HeroFacing ? 1 : 0;	// NB: g_HeroFacing = 0/36 (offset frame LEFT), NON 0/1!
			g_ItX[i] = g_HeroFacing ? (u16)(g_HeroX - 2) : (u16)(g_HeroX + 34);
			g_ItY[i] = g_HeroY + 16;			// hand height
			g_ItTimer[i] = 44;					// range: 44 x 4px = 176px
			g_ItBreak[i] = vy;					// 0 = dritto, 1 = sale, 2 = scende
			g_ItPop[i] = 0;
			vy++;
		}
}

// The crt0 does NOT clear BSS: globals without an explicit initializer hold
// RAM garbage on a cold boot — zero the whole game state by hand
void InitState()
{
	g_Phase = 0;
	g_Zenny = 0;
	g_ZennyShown = 0xFFFF;
	g_ArmorShown = 0xFF;
	g_SeamK[0] = g_SeamK[1] = g_SeamK[2] = 0;
	g_SeamC0[0] = g_SeamC0[1] = g_SeamC0[2] = 0;
	g_WalkTick = 0;
	g_WalkPhase = 0;
	g_NoGrab = 0;
	g_Coyote = 0;
	g_GravTick = 0;
	g_SlotDirty[0] = g_SlotDirty[1] = g_SlotDirty[2] = 0;
	for (u8 e = 0; e < N_ORC; e++)
	{
		g_OrcPK[e] = 0xFFFF;
		g_OrcAtk[e] = 0;
		g_OrcDying[e] = 0;
		g_OrcResp[e] = 0;
		g_OrcFrame[e] = 0;
	}
	for (u8 i = 0; i < N_ITEM; i++)
	{
		g_ItType[i] = IT_NONE;
		g_ItBreak[i] = 0;
		g_ItTimer[i] = 0;
		g_ItPop[i] = 0;
		g_ItCell[i] = 0xFF;
	}
}

// digit scratch in statics: SDCC stack locals in this function overlapped
// (dig[] was trashed by buf[] writes between rows — ix-frame layout bug)
u8 g_HudDig[6];
u8 g_HudBuf[20];

// Scorebar in the top band (page 3, VRAM lines 992-1007), updated only on
// value change. The band is displayed by the ISR split every frame: one
// single copy, zero per-page cost, nothing pinned to the camera anymore.
void UpdateHud()
{
	if (g_Zenny != g_ZennyShown)
	{
		g_ZennyShown = g_Zenny;
		u16 v = g_Zenny;
		for (u8 k = 5; k-- > 0;) { g_HudDig[k] = v % 10; v /= 10; }
		SET_BANK_SEGMENT(3, HUD_BIN_SEG);
		for (u8 r = 0; r < 12; r++)
		{
			const u8* row = (const u8*)(BANK3_BASE + 2048) + (u16)r * 40;
			u8* d = g_HudPix + (u16)r * 20;
			for (u8 k = 0; k < 5; k++)
			{
				const u8* s = row + (g_HudDig[k] << 2);
				d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
				d += 4;
			}
		}
		SET_BANK_SEGMENT(3, 3);
		for (u8 r = 0; r < 12; r++)
		{
			u32 a = HUD_VRAM + (u32)(2 + r) * 128 + 28;	// x=56, lines 994-1005
			VDP_WriteVRAM_128K(g_HudPix + (u16)r * 20, (u16)(a & 0xFFFF), (u8)(a >> 16), 20);
		}
	}
	if (g_HeroArmor != g_ArmorShown)
	{
		g_ArmorShown = g_HeroArmor;
		// armor indicator: filled block right of the ARMOR label
		CmdHMMV(200, 994, 16, 12, g_HeroArmor ? 0x99 : 0x11);
	}
}

void SpawnZenny(u16 x, u16 y)
{
	if ((x & 255) > 236) x -= 16;	// keep clear of the page seam dead zone
	for (u8 i = 0; i < N_ITEM; i++)
		if (g_ItType[i] == IT_NONE)
		{
			g_ItType[i] = IT_ZENNY;
			g_ItX[i] = x;
			g_ItY[i] = y;
			g_ItTimer[i] = 300;
			g_ItPop[i] = 16;
			return;
		}
}

void ItemLogic()
{
	for (u8 i = 0; i < N_ITEM; i++)
	{
		u8 t = g_ItType[i];
		if (t == IT_NONE) { g_ItCell[i] = 0xFF; continue; }
		if (g_ItOnlyDyn && t != IT_DAGGER)
			continue;				// vasi/zenny/armatura: basta 1 passo a frame
		if (t == IT_VASE)
		{
			if (g_ItBreak[i])
			{
				if (--g_ItBreak[i] == 0)
				{
					// reveal the content: pops out, briefly not collectable
					// (or an adjacent hero would swallow it invisibly)
					t = g_ItContent[i];
					g_ItType[i] = t;
					if (t == IT_ARMOR) { g_ItX[i] -= 8; g_ItY[i] -= 16; }
					g_ItCell[i] = (t == IT_ARMOR) ? CELL_ARMOR : 3;
					g_ItPop[i] = 32;
					continue;
				}
				g_ItCell[i] = (g_ItBreak[i] > 15) ? 1 : 2;
				continue;
			}
			g_ItCell[i] = 0;
			// mace hit cracks it open
			if (g_AtkR && g_ItX[i] + 16 > g_AtkL && g_ItX[i] < g_AtkR
			    && g_ItY[i] + 16 > g_HeroY + 8 && g_ItY[i] < g_HeroY + HERO_H)
				g_ItBreak[i] = 30;
			continue;
		}
		if (t == IT_DAGGER)
		{
			g_ItCell[i] = 4 + g_ItContent[i];		// encoder cells 4/5
			u16 dx2 = g_ItX[i];
			if (g_ItContent[i]) { if (dx2 < 4) { g_ItType[i] = IT_NONE; g_ItCell[i] = 0xFF; continue; } dx2 -= 4; }
			else dx2 += 4;
			g_ItX[i] = dx2;
			if (g_ItBreak[i] == 1) g_ItY[i]--;		// fan: drifts up
			else if (g_ItBreak[i] == 2) g_ItY[i]++;	// fan: drifts down
			u16 tip = g_ItContent[i] ? dx2 : (dx2 + 15);
			if (--g_ItTimer[i] == 0 || g_ItY[i] < 24 || g_ItY[i] > 184
			    || CellSolid(tip, g_ItY[i] + 8))
			{ g_ItType[i] = IT_NONE; g_ItCell[i] = 0xFF; continue; }
			// orc hit: same kill path as the mace
			for (u8 e = 0; e < N_ORC; e++)
				if (g_OrcAlive[e] && !g_OrcDying[e]
				    && tip + 4 > g_OrcX[e] + 10 && g_OrcX[e] + 38 > dx2
				    && g_ItY[i] + 12 > g_OrcY[e] && g_OrcY[e] + ORC_H > g_ItY[i])
				{
					g_OrcDying[e] = 48;
					g_ItType[i] = IT_NONE; g_ItCell[i] = 0xFF;
					break;
				}
			if (g_ItType[i] == IT_NONE) continue;
			// vase hit: cracks it open like the mace
			for (u8 v = 0; v < N_ITEM; v++)
				if (g_ItType[v] == IT_VASE && !g_ItBreak[v]
				    && dx2 + 16 > g_ItX[v] && g_ItX[v] + 16 > dx2
				    && g_ItY[i] + 12 > g_ItY[v] && g_ItY[v] + 16 > g_ItY[i])
				{
					g_ItBreak[v] = 30;
					g_ItType[i] = IT_NONE; g_ItCell[i] = 0xFF;
					break;
				}
			continue;
		}
		if (t == IT_ZENNY && g_ItTimer[i] && --g_ItTimer[i] == 0)
		{ g_ItType[i] = IT_NONE; g_ItCell[i] = 0xFF; continue; }
		u8 wpx = (t == IT_ARMOR) ? 32 : 16;
		g_ItCell[i] = (t == IT_ARMOR) ? CELL_ARMOR : 3;
		if (g_ItPop[i]) { g_ItPop[i]--; continue; }
		// hero pickup
		if (!g_HeroDead
		    && g_HeroX + 40 > g_ItX[i] && g_ItX[i] + wpx > g_HeroX + 8
		    && g_HeroY + HERO_H > g_ItY[i] && g_ItY[i] + wpx > g_HeroY)
		{
			if (t == IT_ARMOR) g_HeroArmor = 1;
			else g_Zenny += 50;
			g_ItType[i] = IT_NONE;
			g_ItCell[i] = 0xFF;
		}
	}
}

// Bitmask of the page slots covered by a box at page x px, width wpx
u16 SlotMask(u8 px, u8 wpx)
{
	u8 s0 = px >> 4;
	u8 n = (u8)(((px & 15) + wpx + 15) >> 4);
	u16 m = 0;
	for (u8 i = 0; i < n; i++)
		m |= (u16)1 << ((s0 + i) & 15);
	return m;
}

// Conservative page-coord rect overlap (full cell sizes).
// ASM: aritmetica 8-bit col carry come nono bit (le somme x+w arrivano a
// 288) + short-circuit — ~100 cicli contro i ~400 del C con (u16).
// Convenzione SDCC verificata: ax=A, ay=L, aw..bh = 4..9(ix).
u8 BoxOverlap(u8 ax, u8 ay, u8 aw, u8 ah, u8 bx, u8 by, u8 bw, u8 bh) __naked
{
	ax; ay; aw; ah; bx; by; bw; bh;
	__asm
		push	ix
		ld	ix, #0
		add	ix, sp
		ld	c, a				; c = ax
		ld	b, l				; b = ay
		add	a, 4 (ix)			; ax + aw (9 bit nel carry)
		jr	c, 100001$
		cp	a, 6 (ix)			; sum <= bx -> niente overlap
		jr	c, 100009$
		jr	z, 100009$
	100001$:
		ld	a, 6 (ix)
		add	a, 8 (ix)			; bx + bw
		jr	c, 100002$
		cp	a, c				; sum <= ax -> niente overlap
		jr	c, 100009$
		jr	z, 100009$
	100002$:
		ld	a, b
		add	a, 5 (ix)			; ay + ah
		jr	c, 100003$
		cp	a, 7 (ix)
		jr	c, 100009$
		jr	z, 100009$
	100003$:
		ld	a, 7 (ix)
		add	a, 9 (ix)			; by + bh
		jr	c, 100004$
		cp	a, b
		jr	c, 100009$
		jr	z, 100009$
	100004$:
		ld	a, #1
		pop	ix
		ret
	100009$:
		xor	a, a
		pop	ix
		ret
	__endasm;
}

void PageObjects(u8 w)
{
	// skip flags: page already shows this exact pose at this position
	u8 hx = (u8)(g_HeroX & 255);
	u8 hy = (u8)(g_HeroY & 255);
	// blink: post-hit invulnerability, or the tail of the death sequence
	u8 heroHide = ((g_Invuln & 8) != 0)
	           || (g_HeroDead && g_HeroDead < 32 && (g_HeroDead & 8));
	u8 heroSkip = !heroHide
	           && (g_HeroDrawn[w] == g_HeroFrame)
	           && (g_HXp[w] == hx) && (g_HYp[w] == hy);
	u8 orcSkip[N_ORC];
	u8 orcPx[N_ORC], orcVis[N_ORC];
	for (u8 e = 0; e < N_ORC; e++)
	{
		orcPx[e] = (u8)(g_OrcX[e] & 255);
		orcVis[e] = g_OrcAlive[e]
		         && !(g_OrcDying[e] & 8)
		         && (g_OrcX[e] + ORC_W > g_WX) && (g_OrcX[e] < g_WX + 256);
		u8 clipped = orcVis[e]
		          && ((g_OrcX[e] < g_WX) || (g_OrcX[e] + ORC_W > g_WX + 256));
		orcSkip[e] = orcVis[e] && !clipped
		          && (g_OrcDrawn[w][e] == g_OrcFrame[e])
		          && (g_OrcXp[w][e] == orcPx[e])
		          && (g_OrcYp[w][e] == (u8)(g_OrcY[e] & 255));
	}

	// items: redraw when cell/pos changed or visibility toggled
	for (u8 i = 0; i < N_ITEM; i++)
	{
		u8 c = g_ItCell[i];
		u8 wpx = (c == CELL_ARMOR) ? 32 : 16;
		g_ItPxT[i] = (u8)(g_ItX[i] & 255);
		g_ItVisT[i] = (c != 0xFF)
		           && (g_ItX[i] >= g_WX) && (g_ItX[i] + wpx <= g_WX + 256)
		           && (g_ItPxT[i] <= 256 - wpx);
		if (g_ItVisT[i])
			g_ItRedraw[i] = (g_ItDrawn[w][i] != c)
			             || (g_ItXp[w][i] != g_ItPxT[i])
			             || (g_ItYp[w][i] != (u8)(g_ItY[i] & 255));
		else
			g_ItRedraw[i] = (g_ItDrawn[w][i] != 0xFF);
	}

	// seam split: when the window phase moved on this page, the seam slot
	// will be repainted below — objects sitting on it must not skip
	u8 sk = (u8)(g_WX & 15);
	u8 sc0 = (u8)(g_WX >> 4);
	u8 seamSlot = (u8)(sc0 & 15);
	u8 seamDraw = (sk != 0) && (g_SeamK[w] != sk || g_SeamC0[w] != sc0);
	if (seamDraw)
		g_SlotDirty[w] |= (u16)1 << seamSlot;

	// streaming rewrote these slots on this page: any object whose box
	// overlaps them lost pixels — its skip is invalid
	if (g_SlotDirty[w])
	{
		if (heroSkip && g_HeroDrawn[w] != 0xFF
		    && (g_SlotDirty[w] & SlotMask(g_HXp[w], HERO_W)))
			heroSkip = 0;
		for (u8 e = 0; e < N_ORC; e++)
			if (orcSkip[e] && g_OrcDrawn[w][e] != 0xFF
			    && (g_SlotDirty[w] & SlotMask(g_OrcXp[w][e], ORC_W)))
				orcSkip[e] = 0;
		for (u8 i = 0; i < N_ITEM; i++)
			if (!g_ItRedraw[i] && g_ItVisT[i] && g_ItDrawn[w][i] != 0xFF
			    && (g_SlotDirty[w] & SlotMask(g_ItXp[w][i], 32)))
				g_ItRedraw[i] = 1;
		g_SlotDirty[w] = 0;
	}

	// puntatori di pagina: SDCC ricalcola w*N_ITEM ad ogni [w][i]
	u8* itd = g_ItDrawn[w];
	u8* itxp = g_ItXp[w];
	u8* ityp = g_ItYp[w];

	// an item being erased/redrawn wipes pixels of whoever overlaps it
	for (u8 i = 0; i < N_ITEM; i++)
	{
		if (!g_ItRedraw[i]) continue;
		u8 ipy = (u8)(g_ItY[i] & 255);
		u8 hit;
		if (heroSkip && g_HeroDrawn[w] != 0xFF)
		{
			hit = 0;
			if (itd[i] != 0xFF)
				hit |= BoxOverlap(itxp[i], ityp[i], 32, 32, g_HXp[w], g_HYp[w], HERO_W, HERO_H);
			if (g_ItVisT[i])
				hit |= BoxOverlap(g_ItPxT[i], ipy, 32, 32, g_HXp[w], g_HYp[w], HERO_W, HERO_H);
			if (hit) heroSkip = 0;
		}
		for (u8 e = 0; e < N_ORC; e++)
		{
			if (!orcSkip[e] || g_OrcDrawn[w][e] == 0xFF) continue;
			hit = 0;
			if (itd[i] != 0xFF)
				hit |= BoxOverlap(itxp[i], ityp[i], 32, 32, g_OrcXp[w][e], g_OrcYp[w][e], ORC_W, ORC_H);
			if (g_ItVisT[i])
				hit |= BoxOverlap(g_ItPxT[i], ipy, 32, 32, g_OrcXp[w][e], g_OrcYp[w][e], ORC_W, ORC_H);
			if (hit) orcSkip[e] = 0;
		}
	}

	// a skip is only valid if NO other object's erase/draw touches the box
	// (an orc walking through the hero would wipe him without this)
	for (u8 pass = 0; pass < 2; pass++)
	{
		if (heroSkip && g_HeroDrawn[w] != 0xFF)
		{
			for (u8 e = 0; e < N_ORC; e++)
			{
				if (orcSkip[e]) continue;
				u8 touch = 0;
				if (g_OrcDrawn[w][e] != 0xFF)
					touch |= BoxOverlap(g_OrcXp[w][e], g_OrcYp[w][e], ORC_W, ORC_H,
					                    g_HXp[w], g_HYp[w], HERO_W, HERO_H);
				if (orcVis[e])
					touch |= BoxOverlap(orcPx[e], (u8)(g_OrcY[e] & 255), ORC_W, ORC_H,
					                    g_HXp[w], g_HYp[w], HERO_W, HERO_H);
				if (touch) heroSkip = 0;
			}
		}
		for (u8 e = 0; e < N_ORC; e++)
		{
			if (!orcSkip[e] || g_OrcDrawn[w][e] == 0xFF) continue;
			u8 touch = 0;
			if (!heroSkip)
			{
				if (g_HeroDrawn[w] != 0xFF)
					touch |= BoxOverlap(g_HXp[w], g_HYp[w], HERO_W, HERO_H,
					                    g_OrcXp[w][e], g_OrcYp[w][e], ORC_W, ORC_H);
				touch |= BoxOverlap(hx, hy, HERO_W, HERO_H,
				                    g_OrcXp[w][e], g_OrcYp[w][e], ORC_W, ORC_H);
			}
			u8 o = e ^ 1;			// the other orc (N_ORC == 2)
			if (!orcSkip[o])
			{
				if (g_OrcDrawn[w][o] != 0xFF)
					touch |= BoxOverlap(g_OrcXp[w][o], g_OrcYp[w][o], ORC_W, ORC_H,
					                    g_OrcXp[w][e], g_OrcYp[w][e], ORC_W, ORC_H);
				if (orcVis[o])
					touch |= BoxOverlap(orcPx[o], (u8)(g_OrcY[o] & 255), ORC_W, ORC_H,
					                    g_OrcXp[w][e], g_OrcYp[w][e], ORC_W, ORC_H);
			}
			if (touch) orcSkip[e] = 0;
		}
	}

	// hero/orc erase+redraw wipes overlapping committed items: force their redraw
	for (u8 i = 0; i < N_ITEM; i++)
	{
		if (g_ItRedraw[i] || !g_ItVisT[i] || itd[i] == 0xFF) continue;
		u8 touch = 0;
		u8 ixp = itxp[i], iyp = ityp[i];
		if (!heroSkip)
		{
			if (g_HeroDrawn[w] != 0xFF)
				touch |= BoxOverlap(g_HXp[w], g_HYp[w], HERO_W, HERO_H, ixp, iyp, 32, 32);
			touch |= BoxOverlap(hx, hy, HERO_W, HERO_H, ixp, iyp, 32, 32);
		}
		for (u8 e = 0; e < N_ORC; e++)
		{
			if (orcSkip[e]) continue;
			if (g_OrcDrawn[w][e] != 0xFF)
				touch |= BoxOverlap(g_OrcXp[w][e], g_OrcYp[w][e], ORC_W, ORC_H, ixp, iyp, 32, 32);
			if (orcVis[e])
				touch |= BoxOverlap(orcPx[e], (u8)(g_OrcY[e] & 255), ORC_W, ORC_H, ixp, iyp, 32, 32);
		}
		if (touch) g_ItRedraw[i] = 1;
	}

	// ERASE phase: every object restores its committed box BEFORE any draw
	u16 eraseMask = 0;
	for (u8 i = 0; i < N_ITEM; i++)
		if (g_ItRedraw[i] && itd[i] != 0xFF)
		{
			EraseObj(w, itxp[i], ityp[i],
			         (itd[i] == CELL_ARMOR) ? g_ArmorBoxIdx
			                                         : g_ItBoxIdx[itd[i]]);
			eraseMask |= SlotMask((u8)(itxp[i]), 32);
		}
	for (u8 e = 0; e < N_ORC; e++)
		if (!orcSkip[e] && g_OrcDrawn[w][e] != 0xFF)
		{
			EraseObj(w, g_OrcXp[w][e], g_OrcYp[w][e], g_OrcBox[g_OrcDrawn[w][e]]);
			eraseMask |= SlotMask(g_OrcXp[w][e], ORC_W);
		}
	if (!heroSkip && g_HeroDrawn[w] != 0xFF)
	{
		EraseObj(w, g_HXp[w], g_HYp[w], g_HeroBox[g_HeroDrawn[w]]);
		eraseMask |= SlotMask(g_HXp[w], HERO_W);
	}

	// repaint the seam slot (split content) after the erases wiped over it
	if (sk != 0 && (eraseMask & ((u16)1 << seamSlot)))
		seamDraw = 1;
	if (seamDraw)
	{
		// delta possibile solo per scroll puro (stesso c0, k valido, e
		// nessuna erase ha appena bucato lo slot della cucitura)
		if (g_SeamC0[w] == sc0 && g_SeamK[w] != 0 && g_SeamK[w] <= 15
		    && !(eraseMask & ((u16)1 << seamSlot)))
			DrawSeamDelta(w, sc0, g_SeamK[w], sk);
		else
			DrawSeamSplit(w, sc0, sk);
		g_SeamK[w] = sk;
		g_SeamC0[w] = sc0;
		// the slot holds MIXED content now: mark it unknown so the level
		// check redraws it in full once it stops being the seam
		g_SlotCol[w][seamSlot] = 0xFF;
	}
	else if (sk == 0)
		g_SeamK[w] = 0;

	// DRAW phase: items under, then orcs, hero on top
	for (u8 i = 0; i < N_ITEM; i++)
	{
		if (!g_ItRedraw[i]) continue;
		if (g_ItVisT[i])
		{
			u8 ipy = (u8)(g_ItY[i] & 255);
			u8 c = g_ItCell[i];
			if (c == CELL_ARMOR)
				DrawItem32(w, g_ItPxT[i], ipy);
			else
				DrawItem16(c, w, g_ItPxT[i], ipy);
			itxp[i] = g_ItPxT[i];
			ityp[i] = ipy;
			itd[i] = c;
		}
		else
			itd[i] = 0xFF;
	}
	for (u8 e = 0; e < N_ORC; e++)
	{
		if (orcSkip[e])
			continue;
		if (orcVis[e])
		{
			u8 py = (u8)(g_OrcY[e] & 255);
			u8 cl = (g_WX > g_OrcX[e]) ? (u8)(g_WX - g_OrcX[e]) : 0;
			u8 cr = (g_OrcX[e] + ORC_W > g_WX + 256)
			      ? (u8)(g_OrcX[e] + ORC_W - g_WX - 256) : 0;
			if (cl || cr)
				DrawOrcSlice(g_OrcFrame[e], w, orcPx[e], py,
				             cl, (u8)(ORC_W - cl - cr));
			else
				DrawOrcAuto(g_OrcFrame[e], w, orcPx[e], py);
			g_OrcXp[w][e] = orcPx[e];
			g_OrcYp[w][e] = py;
			g_OrcDrawn[w][e] = g_OrcFrame[e];
		}
		else
			g_OrcDrawn[w][e] = 0xFF;
	}
	if (!heroSkip)
	{
		if (heroHide)
			g_HeroDrawn[w] = 0xFF;
		else
		{
			DrawHeroAuto(g_HeroFrame, w, hx, hy);
			g_HXp[w] = hx;
			g_HYp[w] = hy;
			g_HeroDrawn[w] = g_HeroFrame;
		}
	}
}

//-----------------------------------------------------------------------------
void main()
{
	// Yamanooto: crt0 leaves ENAR.REGEN on (needed to write OFFR on bank
	// switches) — but with REGEN on, READS of 0x7FFC-0x7FFF return the
	// cart registers instead of ROM: any code/data byte living there gets
	// corrupted at fetch time (layout roulette!). All our segments are
	// < 256 so OFFR stays 0: kill the register window for good. OFFR
	// writes (0x7FFE) become harmless no-ops with REGEN off.
	*((volatile u8*)0x7FFF) = 0;

	VDP_SetMode(VDP_MODE_SCREEN5);
	VDP_SetLineCount(VDP_LINE_192);
	VDP_EnableTransparency(FALSE);
	VDP_DisableSprite();
	VDP_RegWrite(25, R25_MAK);			// mask leftmost 8px (seam cover)
	VDP_SetColor(0);
	VDP_EnableDisplay(FALSE);
	VDP_SetPalette(g_LevelPal);			// VDP_USE_PALETTE16: 16 colors from index 0

	LoadLevelData();
	LoadHud();
	LoadBoxes();

	// la cache LRU va azzerata PRIMA del riempimento iniziale (il crt0 non
	// pulisce la BSS: slot spazzatura = tile neri/casuali)
	BuildCacheSlots();
	g_CacheClock = 0;
	for (u16 i = 0; i < N_TILES; i++) g_TileSlot[i] = 0xFF;
	for (u16 i = 0; i < 256; i++) g_CacheTile[i] = 0xFFFF;

	// initial fill: same view on the 3 buffer pages
	{
		u16 rb = (g_WY >> 4) - 2;	// 2 righe di margine sopra la banda
		for (u8 p = 0; p < 3; p++)
			for (u8 j = 0; j < 16; j++)
			{
				u16 rr = rb + j;
				g_RowSlot[p][(u8)(rr & 15)] = rr;
			}
	}
	for (u8 p = 0; p < 3; p++)
		g_RowBase[p] = g_WY >> 4;
	for (u8 p = 0; p < 3; p++)
	{
		for (u16 c = 0; c < 16; c++)
		{
			u16 col = (g_WX >> 4) + c;
			DrawColumn(p, col);
			g_SlotCol[p][col & 15] = (u8)col;
		}
		g_PX[p] = g_WX;
		g_PY[p] = g_WY;
		g_HeroDrawn[p] = 0xFF;
		for (u8 e = 0; e < N_ORC; e++)
			g_OrcDrawn[p][e] = 0xFF;
	}

	InitState();
	InitOrcs();
	InitItems();
	for (u8 p = 0; p < 3; p++)
		for (u8 i = 0; i < N_ITEM; i++)
			g_ItDrawn[p][i] = 0xFF;

	VDP_SetPage(0);
	VDP_SetHorizontalOffset(g_WX & 255);
	VDP_SetVerticalOffset(g_WY & 255);
	VDP_EnableDisplay(TRUE);
	// IM2 setup: 257-byte vector table at 0xE100 (byte 0xE0 -> vector
	// 0xE0E0), JP trampoline to the handler there. Page 0 stays BIOS ROM —
	// IM2 bypasses 0x0038 entirely.
	{
		u8* tab = (u8*)0xE100;
		for (u16 i = 0; i < 257; i++) tab[i] = 0xE0;
		u8* jp = (u8*)0xE0E0;
		jp[0] = 0xC3;
		*((u16*)(jp + 1)) = (u16)&Im2Handler;
	}
	__asm
		di
		ld a, #0xE1
		ld i, a
		im 2
	__endasm;
	VDP_SetHBlankLine(239);		// FH line: (239 - R23hud 224) = raster 15
	VDP_EnableHBlank(TRUE);
	__asm__("ei");

	u8 lastVbl = (u8)g_VblCnt;
	for (;;)
	{
		g_LoopCnt++;
#if (RASTER_GAUGE)
		VDP_SetColor(15);				// raster gauge ON
#endif

		// 60Hz LOGIC / free-running RENDER: heavy frames (scroll+objects
		// cost ~3 vblanks) used to slow the whole GAME to 1/3 speed. The
		// pure-logic pass now sub-steps once per elapsed vblank, so speed,
		// physics and animation timing stay tuned at 60Hz regardless of
		// the render rate.
		u8 nowVbl = (u8)g_VblCnt;
		u8 elapsed = (u8)(nowVbl - lastVbl);
		lastVbl = nowVbl;
		if (elapsed == 0) elapsed = 1;
		if (elapsed > 6) elapsed = 6;
		for (u8 st = 0; st < elapsed; st++)
		{
			g_ItOnlyDyn = (st != 0);	// 0 = primo substep del frame
			HeroLogic();
			OrcAI();
			ItemLogic();
		}

		u8 w = g_View + 1;
		if (w == 3) w = 0;

		ReconcileRows(w);
		ReconcileSlots(w);
		PageObjects(w);
		UpdateHud();

		g_PX[w] = g_WX;
		g_PY[w] = g_WY;
		g_View = w;

#if (RASTER_GAUGE)
		VDP_SetColor(0);				// raster gauge OFF
#endif

		// page + scroll + curtain applied by the ISR inside vblank
		FlipAndWait(w);
	}
}
