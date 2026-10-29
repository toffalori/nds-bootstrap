// The Cheevotron loading screen (stream sessions): a copy of the launcher's
// splash in its font, colours and rows (launcher/source/ui.c, ui_splash), so
// that from the launcher to the game the start looks like one screen.
//
// Drawn twice, the same way: by the first stage as soon as it knows the
// session (dscheevos_session.c, shown on VRAM_E straight away), and by the
// bootloader's ARM7 into the ESRB screen's buffer (dscheevos_hooks.c), which
// the bootloader's ARM9 shows and keeps its dots moving (main.arm9.c). A
// 256x192 8 bpp bitmap with the palette entries below.
#ifndef CHEEVOTRON_LOADING_H
#define CHEEVOTRON_LOADING_H

#define CHEEVOTRON_LOADING_BACKGROUND 0
#define CHEEVOTRON_LOADING_WORDMARK   1
#define CHEEVOTRON_LOADING_ACCENT     2
#define CHEEVOTRON_LOADING_DIM        3
#define CHEEVOTRON_LOADING_TEXT       4
#define CHEEVOTRON_LOADING_SHADOW     5
#define CHEEVOTRON_LOADING_GOLD       6

// The launcher's white theme, for the entries above
#define CHEEVOTRON_RGB_BACKGROUND RGB15(31, 31, 31)
#define CHEEVOTRON_RGB_WORDMARK   RGB15(0, 11, 15)   // title
#define CHEEVOTRON_RGB_ACCENT     RGB15(0, 15, 19)   // bar, "Loading..."
#define CHEEVOTRON_RGB_DIM        RGB15(15, 15, 16)  // first-start note
#define CHEEVOTRON_RGB_TEXT       RGB15(4, 4, 5)     // game, status
#define CHEEVOTRON_RGB_SHADOW     RGB15(22, 27, 29)  // the title's soft shadow
#define CHEEVOTRON_RGB_GOLD       RGB15(30, 22, 4)   // trophy

// The first stage leaves this word after the bitmap on VRAM_E (main BG at
// 0x06000000, rows 192-255 are never shown) when it put the screen up: the
// bootloader's ARM9 then keeps the top screen as it is instead of resetting it
// to white, and clears the word.
#define CHEEVOTRON_SCREEN_MARK  ((vu32*)0x0600FFF0)
#define CHEEVOTRON_SCREEN_MAGIC 0x534C5643   // 'CVLS'

// The drawing code and its font. Not in the bootloader's ARM9 code
// (main.arm9.c), which takes the constants above only: its code and data must
// fit arm9ram.
#ifndef CHEEVOTRON_LOADING_CONSTANTS_ONLY
#include "cheevotron_font.h"

// One line, centred on whole columns as ui.c's center() does; bytes outside
// ASCII 32-126 show as '?'.
static inline void cheevotronLoadingText(u8* screen, int row, const char* in, u8 color) {
	int n = 0;
	while (in[n] && n < 32) {
		n++;
	}
	u8* dst = screen + row * 8 * 256 + ((32 - n) / 2) * 8;
	for (int i = 0; i < n; i++, dst += 8) {
		u8 c = (u8)in[i];
		if (c < 0x20 || c > 0x7E) {
			c = '?';
		}
		const u8* glyph = cheevotronFont + (c - 0x20) * 8;
		for (int y = 0; y < 8; y++) {
			for (int x = 0; x < 8; x++) {
				if (glyph[y] & BIT(x)) {
					dst[y * 256 + x] = color;
				}
			}
		}
	}
}

// One line at twice the font's size, centred on the pixel, its top at pixel
// row y, moved by dx/dy (the shadow).
static inline void cheevotronLoadingBig(u8* screen, int y, int dx, const char* in, u8 color) {
	int n = 0;
	while (in[n] && n < 16) {
		n++;
	}
	u8* dst = screen + y * 256 + (256 - n * 16) / 2 + dx;
	for (int i = 0; i < n; i++, dst += 16) {
		u8 c = (u8)in[i];
		if (c < 0x20 || c > 0x7E) {
			c = '?';
		}
		const u8* glyph = cheevotronFont + (c - 0x20) * 8;
		for (int gy = 0; gy < 8; gy++) {
			for (int gx = 0; gx < 8; gx++) {
				if (glyph[gy] & BIT(gx)) {
					u8* p = dst + gy * 2 * 256 + gx * 2;
					p[0] = p[1] = p[256] = p[257] = color;
				}
			}
		}
	}
}

// The trophy above the title: 16x16, drawn at twice that, bit 15 the
// leftmost pixel; the highlight pixels stay background.
static const u16 cheevotronTrophy[16] = {
	0x3FFC, 0xFFFF, 0xD7FB, 0xD7FB, 0x57FA, 0x3FFC, 0x1FF8, 0x0FF0,
	0x07E0, 0x03C0, 0x0180, 0x0180, 0x03C0, 0x07E0, 0x0FF0, 0x0000,
};

#define CHEEVOTRON_LOADING_TROPHY_Y 20
#define CHEEVOTRON_LOADING_TITLE_Y  60
#define CHEEVOTRON_LOADING_BAR_Y    84

// What the launcher draws as a bitmap behind its text, and nds-bootstrap's
// copies as part of the whole screen: the trophy, the title with its
// shadow, the bar, and the first-start note at the bottom.
static inline void cheevotronLoadingFrame(u8* screen) {
	for (int y = 0; y < 16; y++) {
		for (int x = 0; x < 16; x++) {
			if (cheevotronTrophy[y] & (0x8000 >> x)) {
				u8* p = screen + (CHEEVOTRON_LOADING_TROPHY_Y + y * 2) * 256 + 112 + x * 2;
				p[0] = p[1] = p[256] = p[257] = CHEEVOTRON_LOADING_GOLD;
			}
		}
	}
	cheevotronLoadingBig(screen, CHEEVOTRON_LOADING_TITLE_Y + 2, 2, "Cheevotron", CHEEVOTRON_LOADING_SHADOW);
	cheevotronLoadingBig(screen, CHEEVOTRON_LOADING_TITLE_Y, 0, "Cheevotron", CHEEVOTRON_LOADING_WORDMARK);
	for (int y = CHEEVOTRON_LOADING_BAR_Y; y < CHEEVOTRON_LOADING_BAR_Y + 2; y++) {
		for (int x = 80; x < 176; x++) {
			screen[y * 256 + x] = CHEEVOTRON_LOADING_ACCENT;
		}
	}
	cheevotronLoadingText(screen, 21, "The first start of a game takes", CHEEVOTRON_LOADING_DIM);
	cheevotronLoadingText(screen, 22, "a little longer. That's normal.", CHEEVOTRON_LOADING_DIM);
}

// The game's name as the launcher shows it: the file name without its
// folders and its extension, its first 30 characters.
static inline void cheevotronLoadingName(char shown[31], const char* romPath) {
	const char* name = romPath;
	for (const char* p = romPath; *p; p++) {
		if (*p == '/') {
			name = p + 1;
		}
	}
	int end = 0;
	while (name[end]) {
		end++;
	}
	for (int i = end - 1; i > 0; i--) {
		if (name[i] == '.') {
			end = i;
			break;
		}
	}
	int n = 0;
	for (; n < end && n < 30; n++) {
		shown[n] = name[n];
	}
	shown[n] = 0;
}

// The whole screen onto a cleared (all background) bitmap in RAM; romPath is
// the session's (DSCHEEVOS_S_ROM_PATH, NUL-terminated). Rows 14 and 16 are
// where the launcher prints the game and "Loading..."; the bootloader's ARM9
// moves the dots (row 16, columns 18-20).
static inline void cheevotronLoadingScreen(u8* screen, const char* romPath) {
	char shown[31];
	cheevotronLoadingName(shown, romPath);
	cheevotronLoadingFrame(screen);
	cheevotronLoadingText(screen, 14, shown, CHEEVOTRON_LOADING_TEXT);
	cheevotronLoadingText(screen, 16, "Loading...", CHEEVOTRON_LOADING_ACCENT);
}
#endif

#endif
