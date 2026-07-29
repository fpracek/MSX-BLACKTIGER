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
#include "hero_meta.h"
#include "orc_meta.h"

#ifndef BANK3_BASE
#define BANK3_BASE 0xA000
#endif

#define RASTER_GAUGE	0			// 1 = border color shows frame budget (debug)
#define TILE_CACHE_Y	768			// page 3 as 16x16 grid of 16px tiles
#define SCROLL_SPEED	2
#define WX_MAX			(MAP_W * 16 - 256)
#define WY_MAX			(MAP_H * 16 - 192)

// Sprite tables in the free 4KB tail of page 3 (tiles end at 0x1F000)
#define SPR_PATTERN		0x1F000UL
#define SPR_ATTRIB		0x1FA00UL	// mode-2 color table auto at -0x200 = 0x1F800
#define CURTAIN_COLOR	1			// near-black palette entry

// world scroll position (pixels)
u16 g_WX = 0;
u16 g_WY = 32;
// per-page committed position
u16 g_PX[3];
u16 g_PY[3];
u8  g_View = 0;
volatile bool g_VSynch = FALSE;

void UpdateCurtain(u8 wy);

// Page-flip handshake: the main loop posts the finished page here and spins;
// the ISR applies page + scroll + curtain INSIDE vblank (no mid-frame tearing).
// The ISR only touches VDP ports while a request is posted, and a request is
// only posted while the main loop spins — so ISR and main never interleave
// register writes.
volatile u8 g_FlipReq = 0xFF;	// page to display, 0xFF = nothing pending
u8 g_FlipX, g_FlipY;			// scroll offsets for that page

void ApplyFlip()
{
	VDP_SetPage(g_FlipReq);
	VDP_SetHorizontalOffset(g_FlipX);
	VDP_SetVerticalOffset(g_FlipY);
	UpdateCurtain(g_FlipY);		// sprites shift with R#23
	g_FlipReq = 0xFF;
}

//-----------------------------------------------------------------------------
// Called by the crt0 RAM ISR on VDP interrupt (InstallRAMISR)
void VDP_InterruptHandler()
{
	if (g_FlipReq != 0xFF)
		ApplyFlip();
	g_VSynch = TRUE;
}

// Post the flip and wait for it to be displayed. Fallback: on machines where
// the RAM ISR never runs (C-BIOS), the BIOS keeps ticking JIFFY — apply the
// flip ourselves at the tick, which lands right after vblank start.
#define JIFFY (*(volatile u16*)0xFC9E)

void FlipAndWait(u8 w)
{
	g_FlipX = (u8)(g_PX[w] & 255);
	g_FlipY = (u8)(g_PY[w] & 255);
	g_FlipReq = w;
	u16 j = JIFFY;
	while (g_FlipReq != 0xFF)
	{
		if (JIFFY != j)
		{
			__asm__("di");
			if (g_FlipReq != 0xFF)
				ApplyFlip();
			__asm__("ei");
		}
	}
}

//-----------------------------------------------------------------------------
// Copy tileset from ROM segments to VRAM page 3 (tile cache)
void LoadTiles()
{
	u32 base = (u32)3 * 0x8000;
	for (u8 i = 0; i < 4; i++)
	{
		u16 sz = (i == 3) ? (TILES_BIN_SIZE - 3 * 8192) : 8192;
		SET_BANK_SEGMENT(3, TILES_BIN_SEG + i);
		u32 addr = base + (u32)i * 8192;
		VDP_WriteVRAM_128K((u8*)BANK3_BASE, (u16)(addr & 0xFFFF), (u8)(addr >> 16), sz);
	}
	SET_BANK_SEGMENT(3, 3);		// restore default segment
}

//-----------------------------------------------------------------------------
// Draw one map tile via HMMM from cache to a buffer page
inline void DrawTile(u8 tile, u16 dx, u16 dy)
{
	VDP_CommandHMMM((tile & 15) << 4, TILE_CACHE_Y + ((tile >> 4) << 4), dx, dy, 16, 16);
}

// Draw 8 rows (r0..r0+7) of a map column on page p at world tile column ctx.
// Buffer slot is (ctx & 15): ctx and ctx+16 share the same slot (seam column).
// Runs of flat (uniform) tiles use a single fast HMMV fill instead of
// per-tile cache copies (63% of the map is flat — big streaming saving).
u16 g_SlotDirty[3];				// slots rewritten by streaming, per page

void DrawColumnPart(u8 p, u16 ctx, u8 r0)
{
	g_SlotDirty[p] |= (u16)1 << (ctx & 15);
	const u8* m = g_Map + ctx + ((r0) ? (u16)r0 * MAP_W : 0);
	u16 dx = (ctx & 15) << 4;
	u16 dyBase = (u16)p << 8;
	u8 r = r0;
	u8 rEnd = r0 + 8;
	while (r < rEnd)
	{
		u8 t = *m;
		u8 f = g_TileFlat[t];
		if (f != 0xFF)
		{
			u8 run = 1;
			const u8* m2 = m + MAP_W;
			while ((r + run) < rEnd && g_TileFlat[*m2] == f)
			{
				run++;
				m2 += MAP_W;
			}
			VDP_CommandHMMV(dx, dyBase + ((u16)r << 4), 16, (u16)run << 4, f);
			r += run;
			m = m2;
		}
		else
		{
			DrawTile(t, dx, dyBase + ((u16)r << 4));
			r++;
			m += MAP_W;
		}
	}
}

void DrawColumn(u8 p, u16 ctx)
{
	DrawColumnPart(p, ctx, 0);
	DrawColumnPart(p, ctx, 8);
}

// Deferred bottom half of the last streamed column, per page. The growing
// exposed sliver of a half-drawn column stays under the 8px mask/curtain
// until the half is completed on the page's next write turn.
u16 g_PendCtx[3] = { 0xFFFF, 0xFFFF, 0xFFFF };

//-----------------------------------------------------------------------------
// Right-edge curtain: 12 stacked 16x16 black sprites at x=248 hide the 8px
// where the shared seam column shows stale data (left 8px hidden by R#25 MSK)
// V9938 quirk: sprite Y compare uses the R#23-scrolled line counter, so the
// curtain Y must be re-based on the displayed page's vertical offset.
u8 g_CurtainSAT[49];

void UpdateCurtain(u8 wy)
{
	for (u8 i = 0; i < 12; i++)
	{
		u8 y = (u8)((i << 4) + wy - 1);
		if (y == 216) y = 215;			// 216 = SM2 list terminator, avoid
		g_CurtainSAT[i * 4] = y;
	}
	VDP_WriteVRAM_128K(g_CurtainSAT, (u16)(SPR_ATTRIB & 0xFFFF), (u8)(SPR_ATTRIB >> 16), 49);
}

void InitCurtain()
{
	u8 buf[64];
	u8 i;

	VDP_SetSpritePatternTable(SPR_PATTERN);
	VDP_SetSpriteAttributeTable(SPR_ATTRIB);

	// pattern 0: solid 16x16
	for (i = 0; i < 32; i++) buf[i] = 0xFF;
	VDP_WriteVRAM_128K(buf, (u16)(SPR_PATTERN & 0xFFFF), (u8)(SPR_PATTERN >> 16), 32);

	// mode-2 color table: 12 sprites x 16 lines, fixed color
	for (i = 0; i < 64; i++) buf[i] = CURTAIN_COLOR;
	for (i = 0; i < 3; i++)
		VDP_WriteVRAM_128K(buf, (u16)((SPR_ATTRIB - 0x200 + i * 64) & 0xFFFF), (u8)(SPR_ATTRIB >> 16), 64);

	// attribute table template: y (set per frame), x=248, pattern 0
	for (i = 0; i < 12; i++)
	{
		g_CurtainSAT[i * 4 + 0] = (u8)((i << 4) - 1);
		g_CurtainSAT[i * 4 + 1] = 248;
		g_CurtainSAT[i * 4 + 2] = 0;
		g_CurtainSAT[i * 4 + 3] = 0;
	}
	g_CurtainSAT[48] = 216;				// terminator: disable sprites 12+
	UpdateCurtain(g_WY & 255);

	VDP_RegWriteBakMask(1, (u8)~0x03, 0x02);	// 16x16 sprites, no magnification
	VDP_EnableSprite(TRUE);
}

//-----------------------------------------------------------------------------
// Hero: compiled sprites (SpriteEncoder, 48x48, FN=2 frames per 8KB segment)
// Dispatcher contract (soccer SpriteFrame style): DE = low 14 bits of the
// VRAM address | 0xC000 (write flag), IYl = R#14 bit0, IYh = R#14 bits 1-2.
// Frame code (jump table at 0xA000, stride 4) sets R#14 itself and RETs.
#define HERO_FRAMES		64
#define HERO_W			48
#define HERO_H			48

u8 g_SprR14, g_SprD, g_SprE, g_SprEntry;

void CallHeroFrame() __naked
{
	__asm
		ld	a, (_g_SprR14)
		and	#0x01
		.db	#0xFD
		ld	l, a				; iyl = R14 & 1
		ld	a, (_g_SprR14)
		and	#0x06
		.db	#0xFD
		ld	h, a				; iyh = R14 & 6
		ld	a, (_g_SprD)
		ld	d, a
		ld	a, (_g_SprE)
		ld	e, a
		ld	a, (_g_SprEntry)
		ld	l, a
		ld	h, #0xA0
		jp	(hl)				; frame code RETs to our caller
	__endasm;
}

// x, y in page coords (world & 255). Constraints: x even, x <= 208, y <= 208.
void DrawHero(u8 frame, u8 page, u8 x, u8 y)
{
	SET_BANK_SEGMENT(3, HERO_BIN_SEG + (frame >> 1));
	g_SprR14 = (page << 1) | (y >> 7);
	g_SprD = (u8)(((y & 0x7F) >> 1) | 0xC0);
	g_SprE = (u8)((y << 7) | (x >> 1));
	g_SprEntry = (frame & 1) << 2;
	CallHeroFrame();
	SET_BANK_SEGMENT(3, 3);
}

// Ground truth: the world column currently held by each slot of each page.
// Updated by streaming, read by object erase — they can never disagree.
u8 g_SlotCol[3][16];

// Level-triggered streaming: compare each slot's actual content with what
// the current window requires and rewrite the differences. Self-healing.
// Kept as a SMALL function: inlined in main, SDCC spilled everything to an
// ix frame with shift loops (41% of the frame for 16 compares!).
void ReconcileSlots(u8 w)
{
	u8* sc = g_SlotCol[w];
	u8 c0 = (u8)(g_WX >> 4);
	u8 seam16 = ((g_WX & 15) >= 8);
	for (u8 j = 0; j < 16; j++)
	{
		u8 c = c0 + j;
		if (j == 0 && seam16)
			c = c0 + 16;
		u8 s = c & 15;
		if (sc[s] == c)
			continue;
		sc[s] = c;
		DrawColumnPart(w, c, 0);
		if (g_PendCtx[w] == 0xFFFF)
			g_PendCtx[w] = c;			// defer one bottom half to next frame
		else
			DrawColumnPart(w, c, 8);	// more diffs: complete now
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
	if (tyEnd > MAP_H) tyEnd = MAP_H;
	for (u8 i = 0; i < nCols; i++)
	{
		u8 slot = (s0 + i) & 15;
		u16 c = g_SlotCol[w][slot];
		if (c >= MAP_W)
			continue;
		const u8* mc = g_Map + c + (u16)ty0 * MAP_W;
		u16 dx = (u16)slot << 4;
		u8 ty = ty0;
		while (ty < tyEnd)
		{
			u8 t = *mc;
			u8 f = g_TileFlat[t];
			if (f != 0xFF)
			{
				u8 run = 1;
				const u8* m2 = mc + MAP_W;
				while ((ty + run) < tyEnd && g_TileFlat[*m2] == f)
				{
					run++;
					m2 += MAP_W;
				}
				VDP_CommandHMMV(dx, ((u16)w << 8) + ((u16)ty << 4), 16, (u16)run << 4, f);
				ty += run;
				mc = m2;
			}
			else
			{
				DrawTile(t, dx, ((u16)w << 8) + ((u16)ty << 4));
				ty++;
				mc += MAP_W;
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
// Pose layout: [armored 0-15][bare 16-31][mirrored 32-63]
// per-set order: idle, walk0-5, jump, atk_wind, atk_hit, hurt, die0-2, climb0-1
// deaths: armored die2 = armor shattering; bare die0-2 = collapse -> skeleton
#define HERO_IDLE		0
#define HERO_WALK		1		// +0..5 phase
#define HERO_JUMP		7
#define HERO_ATK_WIND	8		// mace raised (wind-up / recover)
#define HERO_ATK_HIT	9		// lunge, mace extended
#define HERO_HURT		10
#define HERO_DIE		11		// +0..2 death phase
#define HERO_CLIMB		14		// +0..1 grip phase (frontal, on pillars)
#define HERO_BARE		16		// offset of the unarmored set
#define HERO_LEFT		32		// offset of the mirrored (left-facing) frames
#define ATK_FRAMES		16		// attack action duration
#define ATK_REACH		28		// mace hitbox reach beyond the hero box
#define INVULN_FRAMES	60
#define GROUND_Y		128		// hero top y when standing (feet on row 11)
#define CAM_OFFSET		104		// camera keeps hero ~centered
#define CAM_VOFF		120		// vertical anchor: hero ~2/3 down the screen

// VRAM scratch for seam-crossing draws (reserved 3x3 slot block in the cache)
#define SCRATCH_X		208
#define SCRATCH_Y		944

u16 g_HeroX = 96;				// world x (even)
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

u16 g_OrcX[N_ORC] = { 336, 672 };
u16 g_OrcY[N_ORC] = { 144, 144 };	// platform top (192) - ORC_H
i8  g_OrcDir[N_ORC] = { 1, -1 };
u8  g_OrcAlive[N_ORC] = { 1, 1 };
u8  g_OrcDying[N_ORC];			// death blink countdown
u16 g_OrcResp[N_ORC];			// respawn countdown when dead
const u16 g_OrcSpawnX[N_ORC] = { 336, 672 };
u8  g_OrcFrame[N_ORC];
u8  g_OrcAtk[N_ORC];			// axe attack countdown
u8  g_OrcDrawn[3][N_ORC];
u8  g_OrcXp[3][N_ORC], g_OrcYp[3][N_ORC];
u8  g_OrcTick = 0;

void DrawOrc(u8 frame, u8 page, u8 x, u8 y)
{
	SET_BANK_SEGMENT(3, ORC_BIN_SEG + (frame >> 1));	// FN=2
	g_SprR14 = (page << 1) | (y >> 7);
	g_SprD = (u8)(((y & 0x7F) >> 1) | 0xC0);
	g_SprE = (u8)((y << 7) | (x >> 1));
	g_SprEntry = (frame & 1) << 2;
	CallHeroFrame();
	SET_BANK_SEGMENT(3, 3);
}

// --- map collision ---
static const u16 g_RowOff[16] = { 0, 120, 240, 360, 480, 600, 720, 840,
	960, 1080, 1200, 1320, 1440, 1560, 1680, 1800 };

u8 CellSolid(u16 px, u16 py)		// world pixel coords
{
	if (py >= MAP_H * 16)
		return 1;					// safety floor below the map
	return g_Solid[g_RowOff[(u8)(py >> 4)] + (px >> 4)];
}

u8 CellClimb(u16 px, u16 py)		// climbable pillar shaft at world coords
{
	if (py >= MAP_H * 16)
		return 0;
	return g_Climb[g_RowOff[(u8)(py >> 4)] + (px >> 4)];
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
	VDP_CommandHMMV(SCRATCH_X, SCRATCH_Y, HERO_W, HERO_H, 0x00);
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
	VDP_CommandLMMM(SCRATCH_X, SCRATCH_Y, px, dy, w1, HERO_H, VDP_OP_TIMP);
	VDP_CommandLMMM(SCRATCH_X + w1, SCRATCH_Y, 0, dy, HERO_W - w1, HERO_H, VDP_OP_TIMP);
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
	VDP_CommandHMMV(SCRATCH_X, SCRATCH_Y, ORC_W, ORC_H, 0x00);
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
	VDP_CommandLMMM(SCRATCH_X, SCRATCH_Y, px, dy, w1, ORC_H, VDP_OP_TIMP);
	VDP_CommandLMMM(SCRATCH_X + w1, SCRATCH_Y, 0, dy, ORC_W - w1, ORC_H, VDP_OP_TIMP);
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
			g_HeroX = 96;
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
			g_HeroFrame = HERO_CLIMB + ((g_HeroY >> 3) & 1) + cset;
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
	// attack: SPACE starts the mace thrust
	if ((keys & 0x01) && !g_HeroAtk)
		g_HeroAtk = ATK_FRAMES;
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

void PendComplete()
{
	for (u8 p = 0; p < 3; p++)
	{
		if (g_PendCtx[p] != 0xFFFF)
		{
			DrawColumnPart(p, g_PendCtx[p], 8);
			g_PendCtx[p] = 0xFFFF;
		}
	}
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
		    && g_AtkR && g_OrcX[e] + ORC_W > g_AtkL && g_OrcX[e] < g_AtkR
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
		    && g_HeroX + 40 > g_OrcX[e] + 8 && g_OrcX[e] + ORC_W > g_HeroX + 8
		    && g_HeroY + HERO_H > g_OrcY[e] + 4 && g_OrcY[e] + ORC_H > g_HeroY + 8)
			HurtHero(g_OrcX[e]);

		// engagement: 0 = patrol, 1 = charge, 2 = hold (face hero at range)
		u8 mode = 0;
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
					u16 axL, axR;
					if (g_OrcDir[e] > 0) { axL = g_OrcX[e] + 32; axR = g_OrcX[e] + ORC_W + 20; }
					else { axL = (g_OrcX[e] >= 20) ? g_OrcX[e] - 20 : 0; axR = g_OrcX[e] + 16; }
					if (g_HeroX + 40 > axL && axR > g_HeroX + 8
					    && g_HeroY + HERO_H > g_OrcY[e] && g_OrcY[e] + ORC_H > g_HeroY)
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
			u8 blocked = !CellSolid(probe, g_OrcY[e] + ORC_H) ||
			    CellSolid(probe, g_OrcY[e] + ORC_H - 8) ||
			    (g_OrcDir[e] < 0 && g_OrcX[e] <= 2) ||
			    (g_OrcDir[e] > 0 && g_OrcX[e] >= MAP_W * 16 - ORC_W - 2);
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
#define N_ITEM		6
#define IT_NONE		0
#define IT_VASE		1
#define IT_ZENNY	2
#define IT_ARMOR	3

u16 g_ItX[N_ITEM], g_ItY[N_ITEM];
u8  g_ItType[N_ITEM];
u8  g_ItContent[N_ITEM];		// what a vase reveals
u8  g_ItBreak[N_ITEM];			// vase break animation countdown
u16 g_ItTimer[N_ITEM];			// dropped zenny expiry
u8  g_ItCell[N_ITEM];			// gfx cell to show (0xFF = nothing)
u8  g_ItDrawn[3][N_ITEM];		// committed cell per page
u8  g_ItXp[3][N_ITEM], g_ItYp[3][N_ITEM];
u16 g_Zenny;					// wallet (no HUD yet)
const u8 g_ItBox16[4] = { 0, 0, 16, 16 };
const u8 g_ItBox32[4] = { 0, 0, 32, 32 };

// scratch arrays for PageObjects (globals: SDCC ix-frame cost)
u8 g_ItPxT[N_ITEM], g_ItVisT[N_ITEM], g_ItRedraw[N_ITEM];

// Items are compiled sprites like hero/orc: Z80 plots, VDP restores.
// items16.bin (seg ITEMS16, FN=4): cells 0 vase, 1 cracked, 2 shattered, 3 zenny
// armor32.bin (seg ARMOR32, FN=1): the 32x32 armor
void DrawItem16(u8 cell, u8 page, u8 x, u8 y)
{
	SET_BANK_SEGMENT(3, ITEMS16_BIN_SEG);
	g_SprR14 = (page << 1) | (y >> 7);
	g_SprD = (u8)(((y & 0x7F) >> 1) | 0xC0);
	g_SprE = (u8)((y << 7) | (x >> 1));
	g_SprEntry = cell << 2;
	CallHeroFrame();
	SET_BANK_SEGMENT(3, 3);
}

void DrawItem32(u8 page, u8 x, u8 y)
{
	SET_BANK_SEGMENT(3, ARMOR32_BIN_SEG);
	g_SprR14 = (page << 1) | (y >> 7);
	g_SprD = (u8)(((y & 0x7F) >> 1) | 0xC0);
	g_SprE = (u8)((y << 7) | (x >> 1));
	g_SprEntry = 0;
	CallHeroFrame();
	SET_BANK_SEGMENT(3, 3);
}

const u16 kVaseX[3] = { 128, 480, 624 };
const u8  kVaseContent[3] = { IT_ZENNY, IT_ARMOR, IT_ZENNY };

void InitItems()
{
	for (u8 i = 0; i < 3; i++)
	{
		u16 x = kVaseX[i];
		u16 y = 32;
		while (y < MAP_H * 16 - 16 && !CellSolid(x + 8, y + 16)) y += 16;
		g_ItType[i] = IT_VASE;
		g_ItContent[i] = kVaseContent[i];
		g_ItX[i] = x;
		g_ItY[i] = y;
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
			return;
		}
}

void ItemLogic()
{
	for (u8 i = 0; i < N_ITEM; i++)
	{
		u8 t = g_ItType[i];
		if (t == IT_NONE) { g_ItCell[i] = 0xFF; continue; }
		if (t == IT_VASE)
		{
			if (g_ItBreak[i])
			{
				if (--g_ItBreak[i] == 0)
				{
					// reveal the content
					t = g_ItContent[i];
					g_ItType[i] = t;
					if (t == IT_ARMOR) { g_ItX[i] -= 8; g_ItY[i] -= 16; }
					g_ItCell[i] = (t == IT_ARMOR) ? 4 : 3;
					continue;
				}
				g_ItCell[i] = (g_ItBreak[i] > 6) ? 1 : 2;
				continue;
			}
			g_ItCell[i] = 0;
			// mace hit cracks it open
			if (g_AtkR && g_ItX[i] + 16 > g_AtkL && g_ItX[i] < g_AtkR
			    && g_ItY[i] + 16 > g_HeroY + 8 && g_ItY[i] < g_HeroY + HERO_H)
				g_ItBreak[i] = 12;
			continue;
		}
		if (t == IT_ZENNY && g_ItTimer[i] && --g_ItTimer[i] == 0)
		{ g_ItType[i] = IT_NONE; g_ItCell[i] = 0xFF; continue; }
		u8 wpx = (t == IT_ARMOR) ? 32 : 16;
		g_ItCell[i] = (t == IT_ARMOR) ? 4 : 3;
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

// Conservative page-coord rect overlap (full cell sizes)
u8 BoxOverlap(u8 ax, u8 ay, u8 aw, u8 ah, u8 bx, u8 by, u8 bw, u8 bh)
{
	if ((u16)ax + aw <= bx || (u16)bx + bw <= ax) return 0;
	if ((u16)ay + ah <= by || (u16)by + bh <= ay) return 0;
	return 1;
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
		         && (g_OrcX[e] >= g_WX) && (g_OrcX[e] + ORC_W <= g_WX + 256);
		orcSkip[e] = orcVis[e] && (g_OrcDrawn[w][e] == g_OrcFrame[e])
		          && (g_OrcXp[w][e] == orcPx[e])
		          && (g_OrcYp[w][e] == (u8)(g_OrcY[e] & 255));
	}

	// items: redraw when cell/pos changed or visibility toggled
	for (u8 i = 0; i < N_ITEM; i++)
	{
		u8 c = g_ItCell[i];
		u8 wpx = (c == 4) ? 32 : 16;
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

	// an item being erased/redrawn wipes pixels of whoever overlaps it
	for (u8 i = 0; i < N_ITEM; i++)
	{
		if (!g_ItRedraw[i]) continue;
		u8 ipy = (u8)(g_ItY[i] & 255);
		u8 hit;
		if (heroSkip && g_HeroDrawn[w] != 0xFF)
		{
			hit = 0;
			if (g_ItDrawn[w][i] != 0xFF)
				hit |= BoxOverlap(g_ItXp[w][i], g_ItYp[w][i], 32, 32, g_HXp[w], g_HYp[w], HERO_W, HERO_H);
			if (g_ItVisT[i])
				hit |= BoxOverlap(g_ItPxT[i], ipy, 32, 32, g_HXp[w], g_HYp[w], HERO_W, HERO_H);
			if (hit) heroSkip = 0;
		}
		for (u8 e = 0; e < N_ORC; e++)
		{
			if (!orcSkip[e] || g_OrcDrawn[w][e] == 0xFF) continue;
			hit = 0;
			if (g_ItDrawn[w][i] != 0xFF)
				hit |= BoxOverlap(g_ItXp[w][i], g_ItYp[w][i], 32, 32, g_OrcXp[w][e], g_OrcYp[w][e], ORC_W, ORC_H);
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
		if (g_ItRedraw[i] || !g_ItVisT[i] || g_ItDrawn[w][i] == 0xFF) continue;
		u8 touch = 0;
		u8 ixp = g_ItXp[w][i], iyp = g_ItYp[w][i];
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
	for (u8 i = 0; i < N_ITEM; i++)
		if (g_ItRedraw[i] && g_ItDrawn[w][i] != 0xFF)
			EraseObj(w, g_ItXp[w][i], g_ItYp[w][i],
			         (g_ItDrawn[w][i] == 4) ? g_ItBox32 : g_ItBox16);
	for (u8 e = 0; e < N_ORC; e++)
		if (!orcSkip[e] && g_OrcDrawn[w][e] != 0xFF)
			EraseObj(w, g_OrcXp[w][e], g_OrcYp[w][e], g_OrcBox[g_OrcDrawn[w][e]]);
	if (!heroSkip && g_HeroDrawn[w] != 0xFF)
		EraseObj(w, g_HXp[w], g_HYp[w], g_HeroBox[g_HeroDrawn[w]]);

	// DRAW phase: items under, then orcs, hero on top
	for (u8 i = 0; i < N_ITEM; i++)
	{
		if (!g_ItRedraw[i]) continue;
		if (g_ItVisT[i])
		{
			u8 ipy = (u8)(g_ItY[i] & 255);
			u8 c = g_ItCell[i];
			if (c == 4)
				DrawItem32(w, g_ItPxT[i], ipy);
			else
				DrawItem16(c, w, g_ItPxT[i], ipy);
			g_ItXp[w][i] = g_ItPxT[i];
			g_ItYp[w][i] = ipy;
			g_ItDrawn[w][i] = c;
		}
		else
			g_ItDrawn[w][i] = 0xFF;
	}
	for (u8 e = 0; e < N_ORC; e++)
	{
		if (orcSkip[e])
			continue;
		if (orcVis[e])
		{
			u8 py = (u8)(g_OrcY[e] & 255);
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
	VDP_SetMode(VDP_MODE_SCREEN5);
	VDP_SetLineCount(VDP_LINE_192);
	VDP_EnableTransparency(FALSE);
	VDP_DisableSprite();
	VDP_RegWrite(25, R25_MAK);			// mask leftmost 8px (seam cover)
	VDP_SetColor(0);
	VDP_EnableDisplay(FALSE);
	VDP_SetPalette(g_LevelPal);			// VDP_USE_PALETTE16: 16 colors from index 0

	LoadTiles();
	InitCurtain();

	// initial fill: same view on the 3 buffer pages
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

	InitOrcs();
	InitItems();
	for (u8 p = 0; p < 3; p++)
		for (u8 i = 0; i < N_ITEM; i++)
			g_ItDrawn[p][i] = 0xFF;

	VDP_SetPage(0);
	VDP_SetHorizontalOffset(g_WX & 255);
	VDP_SetVerticalOffset(g_WY & 255);
	VDP_EnableDisplay(TRUE);
	__asm__("ei");

	for (;;)
	{
#if (RASTER_GAUGE)
		VDP_SetColor(15);				// raster gauge ON
#endif

		HeroLogic();

		u8 w = g_View + 1;
		if (w == 3) w = 0;

		PendComplete();
		ReconcileSlots(w);
		OrcAI();
		ItemLogic();
		PageObjects(w);

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
