/*
 main.arm9.c

 By Michael Chisholm (Chishm)

 All resetMemory and startBinary functions are based
 on the MultiNDS loader by Darkain.
 Original source available at:
 http://cvs.sourceforge.net/viewcvs.py/ndslib/ndslib/examples/loader/boot/main.cpp

 License:
    NitroHax -- Cheat tool for the Nintendo DS
    Copyright (C) 2008  Michael "Chishm" Chisholm

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#define ARM9
#undef ARM7

#include <stddef.h> // NULL
#include <nds/memory.h>
#include <nds/arm9/background.h>
#include <nds/arm9/video.h>
#include <nds/interrupts.h>
#include <nds/timers.h>
#include <nds/ipc.h>
#include <nds/system.h>
#include <nds/input.h>

#include "common.h"
#include "igm_text.h"
#include "locations.h"
#include "dscheevos_hooks.h"
#include "loading.h"
#define CHEEVOTRON_LOADING_CONSTANTS_ONLY
#include "cheevotron_loading.h"

#define REG_MBK_CACHE_START	0x4004044

extern void arm9_clearCache(void);
extern void arm9_dscheevosCacheOn(void);
extern void arm9_dscheevosCacheOff(void);
extern void arm9code(u32* addr);

u32* arm9executeAddress = NULL;
bool arm9_supportsDSiMode = false;
bool dsiModeConfirmed = false;
static bool colorLutEnabled = false;
bool arm9_boostVram = false;
// Cheevotron: hold both screens white while loading, so the start goes from the
// launcher's Cheevotron screen through white (the loaders) straight to the game,
// with no brand screen in between. White until the ARM7 knows the session is no
// Cheevotron stream session (then the DScheevos brand screen shows). White is
// the zero value on purpose: a stale cache line of the program before once
// zeroed this variable before the ARM9 read it (crt0.arm9.s).
volatile bool arm9_brandScreen = false;
// What the loading screen did, for the ARM7's log: bit 0 the white flag at the
// wait loop's first pass, bit 1 the brand screen was put up, bit 2 the first
// stage's loading screen was kept, bits 8-15 switches between the two.
volatile u32 arm9_screenTrace = 0;
#ifndef LOADERTYPE2
// The first stage's clock (its ARM9 timers 2+3, 32728 Hz) when this code
// started, for the ARM7's log: when the hand-over ended, in stage1.log's time.
volatile u32 arm9_stage1Ticks = 0;
volatile u32 arm9_cardProbe = 0;
volatile u32 arm9_cardProbeFlags = 0;
volatile u32 arm9_cardArgs[4] = {0};

#ifndef LOADERTYPE2
// Cheevotron native cart mode: the DSi keeps a KEY2 session per CPU (measured:
// after the ARM7's handshake an ARM9 read got 0xFFFFFFFF, and knocked the card
// out of step for the ARM7 too). The game reads its card from the ARM9, so
// the ARM9 seeds KEY2 and does every card transfer from then on, as NitroHax
// does. Registers and immediates only: this runs from arm9ram.

// One card transfer: the 8 command bytes (as at 0x040001A8, from two words),
// ROMCTRL, and up to `words` of the reply kept at dest (the rest read out).
static u32 arm9_dscmWordsSeen = 0;   // data words the last transfer delivered (diagnostic)

static void arm9_dscmCardXfer(u32 lo, u32 hi, u32 flags, u32* dest, u32 words) {
	u32 guard = 0x1000000;   // never hang the boot on a card that stops answering
	arm9_dscmWordsSeen = 0;
	*(vu8*)0x040001A1 = 0xC0;              // AUXSPICNT: slot enabled, ROM mode (as libnds' cardWriteCommand)
	for (int i = 0; i < 4; i++) {
		*(vu8*)(0x040001A8 + i) = (u8)(lo >> (i * 8));
		*(vu8*)(0x040001AC + i) = (u8)(hi >> (i * 8));
	}
	*(vu32*)0x040001A4 = flags;            // ROMCTRL: go
	do {
		if (*(vu32*)0x040001A4 & BIT(23)) {  // a word ready
			const u32 data = *(vu32*)0x04100010;
			arm9_dscmWordsSeen++;
			if (words) {
				*dest++ = data;
				words--;
			}
		}
	} while ((*(vu32*)0x040001A4 & BIT(31)) && --guard);
}

// A data-mode chip ID read (0xB8): the card's ID only while the ARM9's KEY2
// session is live (logged before the game starts).
static inline u32 arm9_dscmCardChipId(u32 flags) {
	u32 id = 0xFFFFFFFF;
	arm9_dscmCardXfer(0xB8, 0, flags, &id, 1);
	return id;
}

// The KEY2 part of the handshake, as the ARM7 laid it out (DSCM_CARD_OP_*).
static void arm9_dscmCardScript(const u32* op) {
	for (;;) {
		if (op[0] == DSCM_CARD_OP_XFER) {
			arm9_dscmCardXfer(op[1], op[2], op[3], (u32*)op[4], op[5]);
			op += 6;
		} else if (op[0] == DSCM_CARD_OP_WAIT) {
			u32 lines = op[1];
			u32 guard = 0x4000000;   // bounded, even with no display clock
			u16 last = REG_VCOUNT;
			while (lines && --guard) {
				const u16 now = REG_VCOUNT;
				if (now != last) {
					last = now;
					lines--;
				}
			}
			op += 2;
		} else if (op[0] == DSCM_CARD_OP_SEED) {
			*(vu32*)0x040001A4 = 0;
			*(vu32*)0x040001B0 = op[1];
			*(vu32*)0x040001B4 = op[2];
			*(vu16*)0x040001B8 = (u16)op[3];
			*(vu16*)0x040001BA = (u16)op[4];
			*(vu32*)0x040001A4 = BIT(29) | BIT(15) | BIT(14) | BIT(13);   // nRESET, apply the seed, KEY2 on
			op += 5;
		} else {
			return;
		}
	}
}

// Data reads (0xB7) of `len` bytes from card offset src, a 0x200 block each;
// only the requested words of the last block are stored.
static void arm9_dscmCardRead(u32 src, u32* dest, u32 len, u32 flags) {
	while (len > 0) {
		const u32 n = (len < 0x200) ? len : 0x200;
		const u32 lo = 0xB7 | ((src >> 24) & 0xFF) << 8 | ((src >> 16) & 0xFF) << 16 | ((src >> 8) & 0xFF) << 24;
		arm9_dscmCardXfer(lo, src & 0xFF, flags, dest, (n + 3) >> 2);
		src += n;
		dest += n >> 2;
		len -= n;
	}
}
#endif
#endif
u32 arm9_SCFG_EXT = 0;
u16 arm9_SCFG_CLK = 0;
volatile bool esrbScreenPrepared = false;
volatile bool esrbOnlineNoticeFound = false;
volatile bool esrbOnlineNoticeDisplayed = false;
volatile bool noWhiteFade = false;
volatile bool topScreenFadedIn = false;
volatile bool bottomScreenFadedIn = false;
volatile bool esrbImageLoaded = false;
volatile int arm9_stateFlag = ARM9_BOOT;
static int placardFrames = 0; // DScheevos placard: frames it still stays up (below 0: skipped)

void transferToArm7(int slot) {
	*((vu8*)(REG_MBK_CACHE_START+slot)) |= 0x1;
}

void transferToArm9(int slot) {
	*((vu8*)(REG_MBK_CACHE_START+slot)) &= 0xFE;
}

void initMBKARM9(void) {
	// Default DSiWare settings

	// WRAM-B fully mapped to arm7 // inverted order
	*(vu32*)REG_MBK2 = 0x9195999D;
	*(vu32*)REG_MBK3 = 0x8185898D;

	// WRAM-C fully mapped to arm7 // inverted order
	*(vu32*)REG_MBK4 = 0x9195999D;
	*(vu32*)REG_MBK5 = 0x8185898D;

	// WRAM-A not mapped (reserved to arm7)
	REG_MBK6 = 0x00000000;
	// WRAM-B mapped to the 0x3740000 - 0x37BFFFF area : 512k // why? only 256k real memory is there
	REG_MBK7 = 0x07C03740; // Same as DSiWare
	// WRAM-C mapped to the 0x3700000 - 0x373FFFF area : 256k
	REG_MBK8 = 0x07403700; // Same as DSiWare
}

void initMBKARM9_dsiMode(void) {
	*(vu32*)REG_MBK1 = *(u32*)0x02FFE180;
	*(vu32*)REG_MBK2 = *(u32*)0x02FFE184;
	*(vu32*)REG_MBK3 = *(u32*)0x02FFE188;
	*(vu32*)REG_MBK4 = *(u32*)0x02FFE18C;
	*(vu32*)REG_MBK5 = *(u32*)0x02FFE190;
	REG_MBK6 = *(u32*)0x02FFE194;
	REG_MBK7 = *(u32*)0x02FFE198;
	REG_MBK8 = *(u32*)0x02FFE19C;
	WRAM_CR = *(u8*)0x02FFE1AF;
}

void SetBrightness(u8 screen, s8 bright) {
	if (noWhiteFade && bright > 0) {
		bright -= bright*2;
	}

	u16 mode = 1 << 14;

	if (bright < 0) {
		mode = 2 << 14;
		bright = -bright;
	}
	if (bright > 31) {
		bright = 31;
	}
	*(vu16*)(0x0400006C + (0x1000 * screen)) = bright + mode;
}

void fadeIn(u8 screens) {
	for (int i = 25; i >= 0; i--) {
		if (screens & BIT(0)) {
			SetBrightness(0, i);
		}
		if (screens & BIT(1)) {
			SetBrightness(1, i);
		}
		while (REG_VCOUNT != 191);
		while (REG_VCOUNT == 191);
	}
}

void fadeOut(u8 screens) {
	for (int i = 0; i <= 25; i++) {
		if (screens & BIT(0)) {
			SetBrightness(0, i);
		}
		if (screens & BIT(1)) {
			SetBrightness(1, i);
		}
		while (REG_VCOUNT != 191);
		while (REG_VCOUNT == 191);
	}
}

// DScheevos placard: one frame less per VBlank; A, B or START ends it
#define PLACARD_KEYS (KEY_A | KEY_B | KEY_START)
static void __attribute__((noinline)) placardTick(void) {
	if ((REG_KEYINPUT & PLACARD_KEYS) != PLACARD_KEYS) {
		placardFrames = -60*2; // then wait up to 2 s for the key to be let go
	} else if (REG_VCOUNT == 191) {
		placardFrames--;
		while (REG_VCOUNT == 191);
	}
}

// The ARM7 drew the Cheevotron loading screen at IMAGES_LOCATION (256x192,
// 8 bpp; dscheevos_hooks.c, dscheevosLoadingScreen).
volatile bool arm9_loadingScreenReady = false;

// Cheevotron loading screen (arm9_brandScreen false): white, then the ARM7's copy of
// the launcher's screen with its "Loading..." dots kept moving, so a slow
// start doesn't look like a hang. This code runs from arm9ram alone: only
// immediate constants and plain loops, no tables and no division (check_arm9.sh).
// Not in bootloaderi2: its ARM9 code has 4 KB, and only some DSiWare use it.
#ifndef LOADERTYPE2
// Palettes and power: re-asserted every pass of the wait loop, as the loaders
// reset video while they work. The launcher's white theme (launcher/source/ui.c);
// entries as dscheevos_hooks.c draws them.
static void holdLoadingScreen(void) {
	BG_PALETTE[CHEEVOTRON_LOADING_BACKGROUND] = CHEEVOTRON_RGB_BACKGROUND;
	BG_PALETTE[CHEEVOTRON_LOADING_WORDMARK] = CHEEVOTRON_RGB_WORDMARK;
	BG_PALETTE[CHEEVOTRON_LOADING_ACCENT] = CHEEVOTRON_RGB_ACCENT;
	BG_PALETTE[CHEEVOTRON_LOADING_DIM] = CHEEVOTRON_RGB_DIM;
	BG_PALETTE[CHEEVOTRON_LOADING_TEXT] = CHEEVOTRON_RGB_TEXT;
	BG_PALETTE[CHEEVOTRON_LOADING_SHADOW] = CHEEVOTRON_RGB_SHADOW;
	BG_PALETTE[CHEEVOTRON_LOADING_GOLD] = CHEEVOTRON_RGB_GOLD;
	BG_PALETTE_SUB[0] = CHEEVOTRON_RGB_BACKGROUND;
	REG_POWERCNT |= BIT(1);   // 2D engine A: the top screen
}

// The dots of "Loading..." (row 16, columns 18-20): the font's '.' is the two
// pixels at x+2, y+6 of its cell, so one 16-bit VRAM write each (VRAM takes no
// byte writes): halfword 73 + 4*i of pixel row 134.
static void drawLoadingDots(int dots) {
	vu16* row = (vu16*)0x06000000 + 134 * 128;
	for (int i = 0; i < 3; i++) {
		row[73 + i * 4] = (i < dots) ? 0x0202 : 0x0000;
	}
}
#endif

void waitFrames(int frames) {
	for (int i = 0; i < frames; i++) {
		while (REG_VCOUNT != 191);
		while (REG_VCOUNT == 191);
	}
}

/*-------------------------------------------------------------------------
arm9_main

Clears the ARM9's DMA channels and resets video memory
Jumps to the ARM9 NDS binary in sync with the display and ARM7
Written by Darkain.
Modified by Chishm:
 * Changed MultiNDS specific stuff
--------------------------------------------------------------------------*/
void arm9_main(void) {
 	register int i, reg;

#ifndef LOADERTYPE2
	arm9_stage1Ticks = TIMER_DATA(2) | ((u32)TIMER_DATA(3) << 16);   // before the timers are reset below
	// The first stage put the Cheevotron loading screen up (top screen, BG3 on
	// VRAM_E, cheevotron_loading.h): keep the top screen as it is, so the start
	// shows one screen from the launcher to the game, until the ARM7's copy of
	// the same screen replaces it.
	const bool keepTop = (*CHEEVOTRON_SCREEN_MARK == CHEEVOTRON_SCREEN_MAGIC);
	if (keepTop) {
		*CHEEVOTRON_SCREEN_MARK = 0;
		arm9_screenTrace |= 4;
	}
#else
	const bool keepTop = false;
#endif

	// Set shared ram to ARM7
	WRAM_CR = 0x03;
	REG_EXMEMCNT = 0xE880;

	initMBKARM9();

	arm9_stateFlag = ARM9_START;

	REG_IME = 0;
	REG_IE = 0;
	REG_IF = ~0;

	arm9_clearCache();

	for (i = 0; i < 16*1024; i += 4) { // First 16KB
		*(vu32*)(i + 0x00000000) = 0x00000000; // Clear ITCM
		*(vu32*)(i + 0x00800000) = 0x00000000; // Clear DTCM
	}

	for (i = 16*1024; i < 32*1024; i += 4) { // Second 16KB
		*(vu32*)(i + 0x00000000) = 0x00000000; // Clear ITCM
	}

	arm9_stateFlag = ARM9_MEMCLR;

	// Clear out ARM9 DMA channels
	for (i = 0; i < 4; i++) {
		DMA_CR(i) = 0;
		DMA_SRC(i) = 0;
		DMA_DEST(i) = 0;
		TIMER_CR(i) = 0;
		TIMER_DATA(i) = 0;
		for(reg=0; reg<0x1c; reg+=4)*((vu32*)(0x04004104 + ((i*0x1c)+reg))) = 0;//Reset NDMA.
	}

	// Clear out FIFO
	REG_IPC_SYNC = 0;
	REG_IPC_FIFO_CR = IPC_FIFO_ENABLE | IPC_FIFO_SEND_CLEAR;
	REG_IPC_FIFO_CR = 0;

	if (*(u16*)(CARDENGINEI_ARM9_CLUT_BUFFERED_LOCATION+2) == 0xEA00) {
		const u32 flags = *(u32*)(CARDENGINEI_ARM9_CLUT_BUFFERED_LOCATION+4);
		noWhiteFade = ((flags & BIT(0)) || (flags & BIT(1)));
	}

	VRAM_A_CR = 0x80;
	VRAM_B_CR = 0x80;
	// Don't mess with the VRAM used for execution
	//VRAM_C_CR = 0x80;
	//VRAM_D_CR = 0;
	if (!keepTop) {   // kept: VRAM_E stays the top screen's BG, so the clear below misses it
		VRAM_E_CR = 0x80;
	}
	VRAM_F_CR = 0x80;
	VRAM_G_CR = 0x80;
	VRAM_H_CR = 0x80;
	VRAM_I_CR = 0x80;
	if (keepTop) {
		dmaFill9(0, (void*)0x05000200, 2*1024 - 0x200);   // all palettes but the top screen's BG
	} else {
		dmaFill9(0, BG_PALETTE, 2*1024);
	}
	dmaFill9(0, OAM, 2*1024);
	if (!keepTop) {   // kept: as the first stage set them, the same as below
		for (vu16 *p = (vu16*)&REG_DISPCNT; p <= (vu16*)&REG_MASTER_BRIGHT; p++)
		{
			// Skip VCOUNT. Writing to it was setting it to 0 causing a frame to be
			// misrendered. This can also have side effects on 3DS, even though the
			// official TWL_FIRM can recover from it.
			if (p != (vu16*)&REG_VCOUNT)
				*p = 0;
		}
	} else {
		REG_DISPSTAT = 0;   // the first stage's VBlank IRQ
	}
	for (vu16 *p = (vu16*)&REG_DISPCNT_SUB; p <= (vu16*)&REG_MASTER_BRIGHT_SUB; p++)
	{
		*p = 0;
	}
	if (!keepTop) {
		SetBrightness(0, 31);
	}
	SetBrightness(1, 31);
	dmaFill9(0, VRAM_A, 0x20000*3);		// Banks A, B, C
	dmaFill9(0, VRAM_D, 272*1024);		// Banks D (excluded), E, F, G, H, I

	GFX_STATUS = 0;

	//VRAM_A_CR = 0;
	VRAM_B_CR = 0;
	// Don't mess with the ARM7's VRAM
	//VRAM_C_CR = 0;
	//VRAM_D_CR = 0;
	if (!keepTop) {
		VRAM_E_CR = 0;
	}
	VRAM_F_CR = 0;
	VRAM_G_CR = 0;
	VRAM_H_CR = VRAM_ENABLE | VRAM_H_SUB_BG;
	VRAM_I_CR = VRAM_ENABLE | VRAM_I_SUB_BG_0x06208000;

	// DScheevos/Cheevotron: drive BOTH screens from BG3 bitmaps on VRAM the loader
	// never scribbles over (top on VRAM_E, bottom on VRAM_H/I), not the loader's
	// MODE_FB0 scratch (VRAM_A). Filled with a solid brand colour, this is the
	// whole-load loading screen; the wait loop below holds it every frame.
	VRAM_E_CR = VRAM_ENABLE | VRAM_E_MAIN_BG;   // E -> main BG @ 0x06000000
	REG_DISPCNT = MODE_3_2D | DISPLAY_BG3_ACTIVE;
	REG_BG3CNT = (u16)BgSize_B8_256x256;
	REG_BG3PA = 0x0100;
	REG_BG3PD = 0x0100;
	REG_DISPCNT_SUB = MODE_3_2D | DISPLAY_BG3_ACTIVE;
	REG_BG3CNT_SUB = (u16)BgSize_B8_256x256;
	REG_BG3PA_SUB = 0x0100;
	REG_BG3PD_SUB = 0x0100;

	// POWER_LCD | POWER_2D_B | POWER_SWAP_LCDS, and engine A at once when the
	// first stage's top screen is kept (off for a moment, it would show black)
	REG_POWERCNT = keepTop ? (BIT(0) | BIT(1) | BIT(9) | BIT(15)) : (BIT(0) | BIT(9) | BIT(15));

	{
		u16 c = RGB15(2, 3, 9);
#ifndef LOADERTYPE2
		if (!arm9_brandScreen) {   // Cheevotron (until the ARM7 says otherwise): white from the first frame
			c = RGB15(31, 31, 31);
			BG_PALETTE[0] = c;
			BG_PALETTE_SUB[0] = c;
			REG_POWERCNT |= BIT(1);   // engine A on, or the top screen shows black
		}
		if (keepTop) {
			holdLoadingScreen();   // the first stage's palette, unchanged
		} else
#endif
		{
			BG_PALETTE[1] = c;
			dmaFill9(0x01010101, (void*)0x06000000, 0x10000);   // top: VRAM_E BG3 8bpp
		}
		BG_PALETTE_SUB[1] = c;
		dmaFill9(0x01010101, BG_GFX_SUB, 0x10000);          // bottom: VRAM_H/I BG3 8bpp
	}

	esrbOnlineNoticeFound = (*(u32*)IMAGES_LOCATION == 0x494C4E4F); // 'ONLI'

	// Return to passme loop
	//*(vu32*)0x02FFFE04 = (u32)0xE59FF018; // ldr pc, 0x02FFFE24
	//*(vu32*)0x02FFFE24 = (u32)0x02FFFE04; // Set ARM9 Loop address

	//asm volatile(
	//	"\tbx %0\n"
	//	: : "r" (0x02FFFE04)
	//);

	// Set ARM9 state to ready and wait for it to change again
	arm9_stateFlag = ARM9_READY;
	bool whiteShown = false;   // the Cheevotron loading screen is up (not arm9_brandScreen)
	bool screensCleared = false;   // ARM9_SCRNCLR ran: the screens are the game's now
#ifndef LOADERTYPE2
	bool loadingShown = false;   // the ARM7's loading screen is in VRAM_E
	bool tracedFirst = false;
	u16 lastVcount = 0;
	int dotFrames = 0;
	int dots = 3;
#endif
	while (arm9_stateFlag != ARM9_BOOTBIN) {
		// Hold the loading screen every frame: the loader and the module
		// keep resetting video/brightness during the load, so re-assert the BG3
		// bitmaps (top VRAM_E, bottom VRAM_H/I) and normal brightness. The game
		// takes over when the loop exits.
		if (!screensCleared) {
			REG_DISPCNT = MODE_3_2D | DISPLAY_BG3_ACTIVE;
			REG_BG3CNT = (u16)BgSize_B8_256x256;
			REG_DISPCNT_SUB = MODE_3_2D | DISPLAY_BG3_ACTIVE;
			REG_BG3CNT_SUB = (u16)BgSize_B8_256x256;
#ifndef LOADERTYPE2
			if (!tracedFirst) {
				tracedFirst = true;
				arm9_screenTrace |= arm9_brandScreen ? 0 : 1;
			}
			if (arm9_brandScreen == whiteShown) {
				whiteShown = !arm9_brandScreen;
				arm9_screenTrace += 0x100;
				const u32 fill = whiteShown ? 0 : 0x01010101;   // white, or the brand colour
				if (!(keepTop && whiteShown)) {   // the first stage's loading screen stays
					dmaFill9(fill, (void*)0x06000000, 0x10000);   // VRAM_E, 64 KB
				}
				dmaFill9(fill, BG_GFX_SUB, 0xC000);           // VRAM_H+I, 48 KB
				if (!whiteShown) {   // not a Cheevotron session: the DScheevos brand screen
					BG_PALETTE[0] = BG_PALETTE_SUB[0] = 0;
					BG_PALETTE[1] = BG_PALETTE_SUB[1] = RGB15(2, 3, 9);
					arm9_screenTrace |= 2;
				}
			}
			if (whiteShown) {
				holdLoadingScreen();
				if (arm9_loadingScreenReady && !loadingShown) {
					dmaCopy((void*)IMAGES_LOCATION, (void*)0x06000000, 256 * 192);
					loadingShown = true;
				}
				const u16 vcount = REG_VCOUNT;
				if (loadingShown && vcount < lastVcount && ++dotFrames >= 20) {   // every 20th new frame
					dotFrames = 0;
					dots = (dots + 1) & 3;
					drawLoadingDots(dots);
				}
				lastVcount = vcount;
			}
#endif
			SetBrightness(0, 0);
			SetBrightness(1, 0);
			topScreenFadedIn = true;
			bottomScreenFadedIn = true;
		}
		if (placardFrames > 0) {
			placardTick();
		}
		if (arm9_stateFlag == ARM9_SCRNCLR) {
			while (placardFrames > 0) {
				placardTick();
			}
			if (topScreenFadedIn || bottomScreenFadedIn) {
				u8 screens = 0;
				if (topScreenFadedIn) {
					screens |= BIT(0);
				}
				if (bottomScreenFadedIn) {
					screens |= BIT(1);
				}
				if (arm9_brandScreen) {   // white already: a fade would flash the screens' content
					fadeOut(screens);
				}
#ifndef LOADERTYPE2
				else {
					// display off shows white: clearing the palettes and VRAM below
					// would otherwise show a black frame first
					REG_DISPCNT = 0;
					REG_DISPCNT_SUB = 0;
				}
#endif
				dmaFill9(0, BG_PALETTE, 2*1024);
				dmaFill9(0, VRAM_A, 0x18000);		// Bank A
				dmaFill9(0, BG_GFX_SUB, 0xC000);		// Bank H-I
				dmaFill9(0, (void*)0x06000000, 0x10000);	// Bank E: the top loading screen
			}
			screensCleared = true;
			VRAM_E_CR = 0;
			// so the game doesn't see the placard's skip key as a new press
			for (; placardFrames < 0 && (REG_KEYINPUT & PLACARD_KEYS) != PLACARD_KEYS; placardFrames++) {
				waitFrames(1);
			}
			REG_DISPSTAT = 0;
			VRAM_A_CR = 0;
			VRAM_H_CR = 0;
			VRAM_I_CR = 0;
			REG_DISPCNT = 0;
			REG_DISPCNT_SUB = 0;
			REG_BG3CNT_SUB = 0;
			REG_BG3PA_SUB = 0;
			REG_BG3PD_SUB = 0;
			REG_POWERCNT = 0x820F;
			SetBrightness(0, 0);
			SetBrightness(1, 0);
			arm9_stateFlag = ARM9_READY;
		}
		if (arm9_stateFlag == ARM9_DISPESRB) { // Display ESRB rating and descriptor(s) for current title
			if (*(u32*)IMAGES_LOCATION != 0 && !whiteShown) {   // Cheevotron keeps its loading screen
				esrbScreenPrepared = true;
				if (bottomScreenFadedIn && esrbOnlineNoticeFound) {
					fadeOut(BIT(1));
					bottomScreenFadedIn = false;
				}
				arm9_esrbScreen();
				esrbImageLoaded = true;
				u8 screens = BIT(0);
				if (!bottomScreenFadedIn && esrbOnlineNoticeFound) {
					screens |= BIT(1);
					bottomScreenFadedIn = true;
				}
				if (!topScreenFadedIn) {
					fadeIn(screens);
					topScreenFadedIn = true;
					waitFrames(60*3); // Wait a minimum of 3 seconds
				}
				esrbOnlineNoticeDisplayed = esrbOnlineNoticeFound;
			}
			arm9_stateFlag = ARM9_READY;
		}
		if (arm9_stateFlag == ARM9_DISPSCRN) { // Display nds-bootstrap: Please wait
			if (!whiteShown) {   // Cheevotron keeps its loading screen
				arm9_pleaseWaitText();
			}
			if (!bottomScreenFadedIn) {
				fadeIn(BIT(1));
				bottomScreenFadedIn = true;
			}
			arm9_stateFlag = ARM9_READY;
		}
		if (arm9_stateFlag == ARM9_DISPERR) { // Display nds-bootstrap: An error has occurred
			if (esrbOnlineNoticeDisplayed) {
				fadeOut(BIT(1));
				bottomScreenFadedIn = false;
			}
			arm9_errorText();
			if (!bottomScreenFadedIn) {
				fadeIn(BIT(1));
				bottomScreenFadedIn = true;
			}
			arm9_stateFlag = ARM9_READY;
		}
		if (arm9_stateFlag == ARM9_DSCHEEVOS_INIT) {
			// DScheevos: set up achievements before the game starts (module switches to its own stack)
			int (*dscheevosInit)(const void*) = *(int (**)(const void*))(DSCHEEVOS_LOCATION + DSCHEEVOS_H_INIT);
			// The bootloader runs with the protection unit off, so without caches:
			// parsing the achievement set that way took over a minute on a DSi.
			// Give the module the caches a game would (main RAM cached through
			// 0x02000000, its 0x0C000000 mirror not), then write everything back
			// and return to the bootloader's uncached state.
			arm9_dscheevosCacheOn();
			dscheevosInit((const void*)DSCHEEVOS_SESSION);
			arm9_dscheevosCacheOff();
			arm9_stateFlag = ARM9_READY;
		}
		if (arm9_stateFlag == ARM9_DSCHEEVOS_PLACARD) {
			// DScheevos: the ARM7 drew the session card where the ESRB screen goes
			arm9_esrbScreen();
			arm9_stateFlag = ARM9_READY; // the ARM7 goes on loading during the fade-in
			placardFrames = 90; // 1.5 s, including the fade-in
			if (!topScreenFadedIn) {
				fadeIn(BIT(0));
				topScreenFadedIn = true;
				placardFrames -= 26;
			}
		}
#ifndef LOADERTYPE2
		if (arm9_stateFlag == ARM9_DSCM_CARDPROBE) {
			// arm9_cardArgs in: [0] 1 = also hand the bus to the ARM7 and back and
			// read again (into [1]); out: [2] words the first read delivered,
			// [3] ROMCTRL after it
			REG_EXMEMCNT &= ~BIT(11);              // Slot-1 to the ARM9 (it keeps it: its KEY2 session)
			arm9_cardProbe = arm9_dscmCardChipId(arm9_cardProbeFlags);
			arm9_cardArgs[2] = arm9_dscmWordsSeen;
			arm9_cardArgs[3] = *(vu32*)0x040001A4;
			if (arm9_cardArgs[0] == 1) {
				REG_EXMEMCNT |= BIT(11);
				REG_EXMEMCNT &= ~BIT(11);
				arm9_cardArgs[1] = arm9_dscmCardChipId(arm9_cardProbeFlags);
			}
			arm9_stateFlag = ARM9_READY;
		}
		if (arm9_stateFlag == ARM9_DSCM_CARDUNRESET) {
			// my_enableSlot1 (ARM7) powers the slot and releases the card's reset
			// on the ARM7's side only (ROMCTRL bit 29). Taking the bus later with
			// the ARM9's bit still low reset the card mid-session (both CPUs then
			// read FFFFFFFF / garbage). Release it here too, before any command.
			REG_EXMEMCNT &= ~BIT(11);
			*(vu8*)0x040001A1 = 0xC0;              // AUXSPICNT: slot enabled, ROM mode
			*(vu32*)0x040001A4 = BIT(29);          // ROMCTRL: nRESET
			REG_EXMEMCNT |= BIT(11);
			arm9_stateFlag = ARM9_READY;
		}
		if (arm9_stateFlag == ARM9_DSCM_CARDTOARM7) {
			REG_EXMEMCNT |= BIT(11);               // the game starts with the ARM7 on Slot-1, as on a stock boot
			arm9_stateFlag = ARM9_READY;
		}
		if (arm9_stateFlag == ARM9_DSCM_CARDSCRIPT) {
			REG_EXMEMCNT &= ~BIT(11);
			arm9_dscmCardScript((const u32*)arm9_cardArgs[0]);
			arm9_stateFlag = ARM9_READY;
		}
		if (arm9_stateFlag == ARM9_DSCM_CARDREAD) {
			REG_EXMEMCNT &= ~BIT(11);
			arm9_dscmCardRead(arm9_cardArgs[0], (u32*)arm9_cardArgs[1], arm9_cardArgs[2], arm9_cardArgs[3]);
			arm9_stateFlag = ARM9_READY;
		}
#endif
		if (arm9_stateFlag == ARM9_INITMBK) {
			initMBKARM9_dsiMode();
			arm9_stateFlag = ARM9_READY;
		}
		if (arm9_stateFlag == ARM9_WRAMONARM7) {
			if (REG_MBK7 != 0) {
				for (int i = 0; i < 16; i++) {
					transferToArm7(i);
				}
			}
			arm9_stateFlag = ARM9_READY;
		}
		if (arm9_stateFlag == ARM9_WRAMONARM9) {
			if (REG_MBK7 != 0) {
				for (int i = 0; i < 16; i++) {
					transferToArm9(i);
				}
				colorLutEnabled = true;
			}
			arm9_stateFlag = ARM9_READY;
		}
		if (arm9_stateFlag == ARM9_SETSCFG) {
			if (dsiModeConfirmed) {
				if (arm9_supportsDSiMode && !colorLutEnabled) {
					initMBKARM9_dsiMode(); // This is needed for camera to init properly in some games, even when called with the ARM9_INITMBK flag prior
				}
				REG_SCFG_EXT = 0x8307F100;
				REG_SCFG_CLK = 0x87;
				REG_SCFG_RST = 1;
				if (!arm9_supportsDSiMode) {
					if (!arm9_boostVram) {
						REG_SCFG_EXT &= ~BIT(13);
					}
					// transferToArm9(15);
					for (int i = 0; i < 16; i++) {
						transferToArm9(i);
					}
				}
			} else {
				arm9_SCFG_EXT = REG_SCFG_EXT = 0x8300C000 | BIT(16);	// Bit 16: NDMA
				arm9_SCFG_CLK = REG_SCFG_CLK;
				// transferToArm9(15);
				for (int i = 0; i < 16; i++) {
					transferToArm9(i);
				}
				// lock SCFG
				REG_SCFG_EXT &= ~(1UL << 31);
			}
			arm9_stateFlag = ARM9_READY;
		}
	}

	// Clear out ARM9 DMA channel 3
	DMA_CR(3) = 0;
	DMA_SRC(3) = 0;
	DMA_DEST(3) = 0;

	while (REG_VCOUNT != 191);
	while (REG_VCOUNT == 191);

	// Start ARM9
	arm9code(arm9executeAddress);
}
