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

// The launcher's white theme, for the entries above
#define CHEEVOTRON_RGB_BACKGROUND RGB15(31, 31, 31)
#define CHEEVOTRON_RGB_WORDMARK   RGB15(0, 11, 15)
#define CHEEVOTRON_RGB_ACCENT     RGB15(0, 15, 19)   // rule, "Loading..."
#define CHEEVOTRON_RGB_DIM        RGB15(13, 13, 14)  // tagline
#define CHEEVOTRON_RGB_TEXT       RGB15(4, 4, 5)     // game, status

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

// The whole screen onto a cleared (all background) bitmap in RAM; romPath is
// the session's (DSCHEEVOS_S_ROM_PATH, NUL-terminated).
static inline void cheevotronLoadingScreen(u8* screen, const char* romPath) {
	const char* name = romPath;
	for (const char* p = romPath; *p; p++) {
		if (*p == '/') {
			name = p + 1;
		}
	}
	char shown[31];   // the launcher shows the file name's first 30 characters
	int n = 0;
	for (; name[n] && n < 30; n++) {
		shown[n] = name[n];
	}
	shown[n] = 0;

	cheevotronLoadingText(screen, 8, "C H E E V O T R O N", CHEEVOTRON_LOADING_WORDMARK);
	cheevotronLoadingText(screen, 9, "--------------------", CHEEVOTRON_LOADING_ACCENT);
	cheevotronLoadingText(screen, 11, "achievements on real hardware", CHEEVOTRON_LOADING_DIM);
	cheevotronLoadingText(screen, 14, shown, CHEEVOTRON_LOADING_TEXT);
	cheevotronLoadingText(screen, 16, "Loading...", CHEEVOTRON_LOADING_ACCENT);
	cheevotronLoadingText(screen, 18, "Starting with achievements...", CHEEVOTRON_LOADING_TEXT);   // the launcher's last line
}
#endif

#endif
