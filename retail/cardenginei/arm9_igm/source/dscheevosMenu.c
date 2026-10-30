/*
 * DScheevos (RetroAchievements) page in the in-game menu: the game, the
 * mode, progress, the unlocks of this session and what is active now, then
 * every achievement of the set, a page at a time. Reads the module's status,
 * list and notices; the module itself lives in the DScheevos repository.
 */
#ifndef B4DS
#include <stddef.h>
#include <nds/ndstypes.h>
#include <nds/input.h>
#include <nds/system.h>

#include "inGameMenu.h"
#include "dscheevos_hooks.h"

#define KEYS sharedAddr[5]
#define LIST_ROWS 10 // two lines each

// The end of the module's RAM in the view p uses: the module may keep its data in
// the 0x0C000000 mirror of the same RAM (uncached on most games), and then the menu
// reads it there too, so both see the same cache lines. 0 if p is in neither view.
static u32 moduleEnd(u32 p) {
	const u32 mirror = DSCHEEVOS_LOCATION + 0x0A000000;
	if (p >= DSCHEEVOS_LOCATION && p < DSCHEEVOS_LOCATION + DSCHEEVOS_SIZE) return DSCHEEVOS_LOCATION + DSCHEEVOS_SIZE;
	if (p >= mirror && p < mirror + DSCHEEVOS_SIZE) return mirror + DSCHEEVOS_SIZE;
	return 0;
}

// A module pointer, if it points inside the module's RAM with room for `size` bytes
static const void* modulePtr(u32 offset, u32 size) {
	const u32 p = *(u32*)(DSCHEEVOS_LOCATION + offset);
	const u32 end = moduleEnd(p);
	if (!end || size > end - p) {
		return NULL;
	}
	return (const void*)p;
}

static const DScheevosStatus* dscheevosStatus(void) {
	if (*(u32*)DSCHEEVOS_LOCATION != DSCHEEVOS_MAGIC) {
		return NULL;
	}
	return (const DScheevosStatus*)modulePtr(DSCHEEVOS_H_STATUS, sizeof(DScheevosStatus));
}

// The published list, or NULL if there's none yet (or it doesn't look right)
static const DScheevosList* dscheevosList(void) {
	const DScheevosList* l = (const DScheevosList*)modulePtr(DSCHEEVOS_H_LIST, sizeof(DScheevosList));
	if (!l || l->magic != DSCHEEVOS_LIST_MAGIC || l->entrySize != sizeof(DScheevosListEntry)
	 || l->count > (moduleEnd((u32)l) - (u32)l - sizeof(DScheevosList)) / sizeof(DScheevosListEntry)) {
		return NULL;
	}
	return l;
}

static const DScheevosNotices* dscheevosNotices(void) {
	const DScheevosNotices* n = (const DScheevosNotices*)modulePtr(DSCHEEVOS_H_NOTICES, sizeof(DScheevosNotices));
	return (n && n->magic == DSCHEEVOS_NOTICES_MAGIC) ? n : NULL;
}

// The game is stopped and the ARM7 is about to copy the journal: let the module
// evaluate the frames still queued first, so an unlock in the last moments is
// in it. The module skips this itself if its idle work is running or parked.
void dscheevosFlush(void) {
	if (*(u32*)DSCHEEVOS_LOCATION != DSCHEEVOS_MAGIC) {
		return;
	}
	const u32 f = *(u32*)(DSCHEEVOS_LOCATION + DSCHEEVOS_H_FLUSH);
	if (f <= DSCHEEVOS_LOCATION || f >= DSCHEEVOS_LOCATION + DSCHEEVOS_IMAGE_MAX) {
		return;    // an older module: no flush entry
	}
	(*(void (*)(void))f)();
}

// The module's "Reconnect" (Cheevotron streamer builds): a fresh Wi-Fi
// association and pairing at its next idle pass. 0 if the module has none.
static u32 dscheevosReconnectEntry(void) {
	if (*(u32*)DSCHEEVOS_LOCATION != DSCHEEVOS_MAGIC) {
		return 0;
	}
	const u32 f = *(u32*)(DSCHEEVOS_LOCATION + DSCHEEVOS_H_RECONNECT);
	return (f > DSCHEEVOS_LOCATION && f < DSCHEEVOS_LOCATION + DSCHEEVOS_IMAGE_MAX) ? f : 0;
}

bool dscheevosHardcore(void) {
	const DScheevosStatus* s = dscheevosStatus();
	return s && s->hardcore;
}

// Copies at most `width` printable characters (UTF-8 and others become '?')
static void fit(unsigned char* out, const char* in, int width) {
	int n = 0;
	while (*in && n < width) {
		const unsigned char c = (unsigned char)*in++;
		if (c >= 0x80 && c < 0xC0) {
			continue; // UTF-8 continuation byte
		}
		out[n++] = (c >= 0x20 && c < 0x7F) ? c : '?';
	}
	out[n] = 0;
}

// Prints text over lines of at most `width`, breaking at spaces; returns the next y
static int printWrapped(int x, int y, int width, int maxY, const char* in, FontPalette palette) {
	unsigned char text[101];
	unsigned char line[33];
	fit(text, in, sizeof(text) - 1);
	const unsigned char* p = text;
	while (*p && y <= maxY) {
		while (*p == ' ') {
			p++;
		}
		int len = 0;
		while (p[len] && len < width) {
			len++;
		}
		if (p[len] && p[len] != ' ') {
			int cut = len;
			while (cut > 0 && p[cut] != ' ') {
				cut--;
			}
			if (cut > 0) {
				len = cut; // else one long word: cut it
			}
		}
		for (int i = 0; i < len; i++) {
			line[i] = p[i];
		}
		line[len] = 0;
		print(x, y++, line, palette, false);
		p += len;
	}
	return y;
}

static int numDigits(u32 val) {
	int digits = 1;
	for (; val >= 10; val /= 10) {
		digits++;
	}
	return digits;
}

// Prints a number at its natural width; returns the x after it
static int printNum(int x, int y, u32 val, FontPalette palette) {
	const int digits = numDigits(val);
	printDec(x, y, val, digits, palette, false);
	return x + digits;
}

// Prints total / count with one decimal ("12.3"); returns the x after it
static int printTenths(int x, int y, u32 total, u32 count, FontPalette palette) {
	const u32 whole = total / count;
	x = printNum(x, y, whole, palette);
	printChar(x, y, '.', palette, false);
	printChar(x + 1, y, '0' + ((total - whole * count) * 10) / count, palette, false);
	return x + 2;
}

// Module self-timing: average/max scanlines a frame (263 a frame, about
// 4,260 ARM9 cycles each at 67 MHz)
static void printLines(int y, const char* label, u32 total, u32 max, u32 count) {
	print(1, y, (const unsigned char*)label, FONT_DARKER_GRAY, false);
	int x = printTenths(16, y, total, count, FONT_DARKER_GRAY);
	printChar(x, y, '/', FONT_DARKER_GRAY, false);
	x = printNum(x + 1, y, max, FONT_DARKER_GRAY);
	print(x + 1, y, (const unsigned char*)"lines", FONT_DARKER_GRAY, false);
}

static void waitFrame(void) {
	while (REG_VCOUNT != 191);
	while (REG_VCOUNT == 191);
}

// Waits for a newly pressed key; held directions repeat
static u32 keysLast;
static int keyDelay;

static u32 getKey(void) {
	const u32 mask = KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_A | KEY_B;
	const u32 dirs = KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT;
	while (1) {
		waitFrame();
		const u32 now = KEYS & mask;
		const u32 pressed = now & ~keysLast;
		if (pressed) {
			keysLast = now;
			keyDelay = 20;
			return pressed;
		}
		keysLast = now;
		if ((now & dirs) && --keyDelay <= 0) {
			keyDelay = 5;
			return now & dirs;
		}
	}
}

static void stateText(const DScheevosListEntry* e, const char** text, FontPalette* pal) {
	switch (e->state) {
		case DSCHEEVOS_ACH_UNLOCKED: *text = "Unlocked"; *pal = FONT_LIME; break;
		case DSCHEEVOS_ACH_UNLOCKED_SOFTCORE: *text = "Casual only"; *pal = FONT_LIGHT_BLUE; break;
		case DSCHEEVOS_ACH_DISABLED: *text = "Disabled"; *pal = FONT_DARKER_GRAY; break;
		default: *text = "Locked"; *pal = FONT_WHITE; break;
	}
}

static void drawSummary(const DScheevosStatus* s, u32 reconnect, bool reconnectAsked) {
	unsigned char line[33];
	int x, y;

	fit(line, s->gameTitle, 30);
	printCenter(16, 1, line, FONT_LIGHT_GRAY, false);

	print(1, 3, (const unsigned char*)(s->hardcore ? "HARDCORE" : "Casual"), s->hardcore ? FONT_RED : FONT_LIGHT_BLUE, false);
	if (s->hardcore) {
		// hardcore rules always apply; whether RetroAchievements counts the
		// unlocks as hardcore depends on it recognising this client
		print(1, 4, (const unsigned char*)(s->hardcore == DSCHEEVOS_HARDCORE_REFUSED ?
			"Rules on, RA counts as Casual" : "Not yet confirmed by RA"), FONT_LIGHT_GRAY, false);
	}
	x = printNum(11, 3, s->achievementsUnlocked, FONT_WHITE);
	printChar(x, 3, '/', FONT_WHITE, false);
	printNum(x + 1, 3, s->achievements, FONT_WHITE);
	x = printNum(20, 3, s->pointsUnlocked, FONT_WHITE);
	printChar(x, 3, '/', FONT_WHITE, false);
	x = printNum(x + 1, 3, s->points, FONT_WHITE);
	print(x + 1, 3, (const unsigned char*)"pts", FONT_LIGHT_GRAY, false);

	print(1, 5, (const unsigned char*)"Unlocked this session", FONT_LIGHT_BLUE, false);
	if (s->unlockCount == 0) {
		print(2, 6, (const unsigned char*)"None yet", FONT_LIGHT_GRAY, false);
	}
	y = 6;
	for (u32 i = 0; i < s->unlockCount && i < 4; i++) {
		// newest first
		const DScheevosUnlock* u = &s->recent[(s->unlockCount - 1 - i) % 8];
		fit(line, u->title, 25);
		print(2, y, line, FONT_WHITE, false);
		printNum(28, y, u->points, FONT_LIME);
		y++;
	}

	// challenges and leaderboard trackers showing now
	const DScheevosNotices* n = dscheevosNotices();
	y = 11;
	for (int i = 0; n && i < DSCHEEVOS_INDICATOR_MAX * 2 && y <= 13; i++) {
		const DScheevosIndicator* ind = (i < DSCHEEVOS_INDICATOR_MAX) ? &n->challenges[i] : &n->trackers[i - DSCHEEVOS_INDICATOR_MAX];
		if (!ind->id) {
			continue;
		}
		if (y == 11) {
			print(1, 10, (const unsigned char*)"Now active", FONT_LIGHT_BLUE, false);
		}
		print(2, y, (const unsigned char*)(i < DSCHEEVOS_INDICATOR_MAX ? "Challenge" : "Leaderboard"), FONT_LIGHT_GRAY, false);
		fit(line, ind->text, 18);
		print(14, y++, line, FONT_WHITE, false);
	}

	if (s->error) {
		print(1, 15, (const unsigned char*)"Achievements stopped (error)", FONT_RED, false);
	} else {
		const char* net = "No network in this session";
		FontPalette pal = FONT_LIGHT_GRAY;
		switch (s->netState) {
			case DSCHEEVOS_NET_CONNECTING: net = "Connecting..."; break;
			case DSCHEEVOS_NET_ONLINE: net = "Online"; pal = FONT_LIME; break;
			case DSCHEEVOS_NET_SENDING: net = "Online, sending..."; pal = FONT_LIME; break;
			case DSCHEEVOS_NET_PAUSED: net = "Paused: game uses wireless"; break;
			case DSCHEEVOS_NET_OFFLINE: net = "Wi-Fi lost"; pal = FONT_RED; break;
			case DSCHEEVOS_NET_ERROR: net = "Can't reach the server"; pal = FONT_RED; break;
		}
		print(1, 15, (const unsigned char*)net, pal, false);
		print(1, 16, (const unsigned char*)"Sent", FONT_LIGHT_GRAY, false);
		x = printNum(6, 16, s->unlocksSent, FONT_WHITE);
		if (s->pendingRequests) {
			print(x + 2, 16, (const unsigned char*)"waiting", FONT_LIGHT_GRAY, false);
			printNum(x + 10, 16, s->pendingRequests, FONT_WHITE);
		}
	}
	y = 17;
	print(1, y, (const unsigned char*)"Frames checked", FONT_DARKER_GRAY, false);
	printNum(16, y++, s->framesEvaluated, FONT_DARKER_GRAY);
	if (s->framesDropped) {
		print(1, y, (const unsigned char*)"Frames skipped", FONT_RED, false);
		printNum(16, y++, s->framesDropped, FONT_RED);
	}
	if (s->netRejoins) {
		print(1, y, (const unsigned char*)"Wi-Fi rejoins", FONT_DARKER_GRAY, false);
		printNum(16, y++, s->netRejoins, FONT_DARKER_GRAY);
	}
	if (s->timedFrames) {
		// what the module costs the game in its IRQ, and what it uses of idle time
		printLines(y++, "IRQ cost", s->irqLinesTotal, s->irqLinesMax, s->timedFrames);
		printLines(y++, "Idle use", s->idleLinesTotal, s->idleLinesMax, s->timedFrames);
		if (s->idleNone && y <= 21) {
			print(1, y, (const unsigned char*)"Frames no idle", FONT_DARKER_GRAY, false);
			printNum(16, y, s->idleNone, FONT_DARKER_GRAY);
		}
	}
	if (reconnectAsked) {
		print(0, 22, (const unsigned char*)"Reconnects in game  B: Back", FONT_LIME, false);
	} else if (reconnect) {
		print(0, 22, (const unsigned char*)"A: Reconnect RIGHT: List B: Back", FONT_LIGHT_GRAY, false);
	} else {
		print(1, 22, (const unsigned char*)"RIGHT: List  B: Back", FONT_LIGHT_GRAY, false);
	}
}

// Cheevotron (the streamer module, the one with a reconnect entry): the phone
// runs the achievements, so the console has no list or progress to show. The
// page says how the link to the phone stood when the menu opened; the game,
// and with it the link, is stopped while the menu is open.
static void drawStream(const DScheevosStatus* s, bool reconnectAsked) {
	const char* phone = "Looking for the phone...";
	FontPalette pal = FONT_LIGHT_GRAY;
	switch (s->netState) {
		case DSCHEEVOS_NET_CONNECTING: phone = "Joining the Wi-Fi..."; break;
		case DSCHEEVOS_NET_PAUSED: phone = "Paused: game uses wireless"; break;
		case DSCHEEVOS_NET_OFFLINE: phone = "Wi-Fi lost"; pal = FONT_RED; break;
		default:
			if (s->netDetail) {   // the streamer's pairing session: nonzero while paired
				phone = "Phone connected";
				pal = FONT_LIME;
			}
			break;
	}
	printCenter(16, 0, (const unsigned char*)"Cheevotron", FONT_WHITE, false);
	print(1, 3, (const unsigned char*)phone, pal, false);
	print(1, 5, (const unsigned char*)"Achievements run on the phone:", FONT_LIGHT_GRAY, false);
	print(1, 6, (const unsigned char*)"the list and your progress", FONT_LIGHT_GRAY, false);
	print(1, 7, (const unsigned char*)"are in the Cheevotron app.", FONT_LIGHT_GRAY, false);
	print(1, 9, (const unsigned char*)"The link waits while this", FONT_DARKER_GRAY, false);
	print(1, 10, (const unsigned char*)"menu is open and comes back", FONT_DARKER_GRAY, false);
	print(1, 11, (const unsigned char*)"by itself in the game.", FONT_DARKER_GRAY, false);
	if (s->netRejoins) {
		print(1, 13, (const unsigned char*)"Wi-Fi rejoins", FONT_DARKER_GRAY, false);
		printNum(16, 13, s->netRejoins, FONT_DARKER_GRAY);
	}
	if (reconnectAsked) {
		print(1, 22, (const unsigned char*)"Reconnects in game  B: Back", FONT_LIME, false);
	} else {
		print(1, 22, (const unsigned char*)"A: Reconnect  B: Back", FONT_LIGHT_GRAY, false);
	}
}

// One page of the list, two lines per achievement; `sel` has the cursor
static void drawList(const DScheevosStatus* s, const DScheevosList* l, u32 sel) {
	unsigned char line[33];
	const char* state;
	FontPalette pal;

	print(1, 0, (const unsigned char*)"Achievements", FONT_WHITE, false);
	if (!l || l->count == 0) {
		const char* msg = "No list";
		if (!l && !s->error && *(u32*)(DSCHEEVOS_LOCATION + DSCHEEVOS_H_LIST) == 0) {
			msg = "Loading achievements...";
		} else if (l) {
			msg = "No achievements in this set";
		}
		print(2, 3, (const unsigned char*)msg, FONT_LIGHT_GRAY, false);
		print(1, 22, (const unsigned char*)"LEFT: Summary  B: Back", FONT_LIGHT_GRAY, false);
		return;
	}

	const u32 page = sel / LIST_ROWS;
	const u32 pages = (l->count + LIST_ROWS - 1) / LIST_ROWS;
	int x = 31 - numDigits(page + 1) - numDigits(pages);
	x = printNum(x, 0, page + 1, FONT_LIGHT_GRAY);
	printChar(x, 0, '/', FONT_LIGHT_GRAY, false);
	printNum(x + 1, 0, pages, FONT_LIGHT_GRAY);

	for (u32 i = page * LIST_ROWS; i < l->count && i < (page + 1) * LIST_ROWS; i++) {
		const DScheevosListEntry* e = &l->e[i];
		const int y = 2 + (i % LIST_ROWS) * 2;
		stateText(e, &state, &pal);
		if (i == sel) {
			printChar(0, y, '>', FONT_WHITE, false);
		}
		fit(line, e->title, 25);
		print(1, y, line, pal, false);
		printNum(32 - numDigits(e->points), y, e->points, FONT_LIGHT_GRAY);
		x = 3;
		print(x, y + 1, (const unsigned char*)state, FONT_DARKER_GRAY, false);
		while (state[x - 3]) {
			x++;
		}
		const bool challenge = (e->flags & DSCHEEVOS_ACH_FLAG_CHALLENGE) != 0;
		if ((e->flags & DSCHEEVOS_ACH_FLAG_MEASURED) && e->state != DSCHEEVOS_ACH_UNLOCKED && e->measured[0]) {
			fit(line, e->measured, (challenge ? 22 : 32) - (x + 2));
			print(x + 2, y + 1, line, FONT_LIGHT_BLUE, false);
		}
		if (challenge) {
			print(23, y + 1, (const unsigned char*)"Challenge", FONT_RED, false);
		}
	}
	print(1, 22, (const unsigned char*)"A: Info  B: Back  <>: Page", FONT_LIGHT_GRAY, false);
}

static void drawDetail(const DScheevosListEntry* e) {
	unsigned char line[33];
	const char* state;
	FontPalette pal;

	stateText(e, &state, &pal);
	int y = printWrapped(1, 0, 30, 3, e->title, FONT_WHITE);
	y++;
	int x = printNum(1, y, e->points, FONT_WHITE);
	print(x + 1, y, (const unsigned char*)(e->points == 1 ? "point" : "points"), FONT_LIGHT_GRAY, false);
	print(x + (e->points == 1 ? 7 : 8), y++, (const unsigned char*)state, pal, false);
	if ((e->flags & DSCHEEVOS_ACH_FLAG_MEASURED) && e->measured[0]) {
		print(1, y, (const unsigned char*)"Progress", FONT_LIGHT_GRAY, false);
		fit(line, e->measured, 15);
		print(10, y++, line, FONT_LIGHT_BLUE, false);
	}
	if (e->flags & DSCHEEVOS_ACH_FLAG_CHALLENGE) {
		print(1, y++, (const unsigned char*)"Challenge active", FONT_RED, false);
	}
	if (e->flags & DSCHEEVOS_ACH_FLAG_MISSABLE) {
		print(1, y++, (const unsigned char*)"Missable", FONT_LIGHT_GRAY, false);
	}
	y++;
	printWrapped(1, y, 30, 20, e->description, FONT_WHITE);
	print(1, 22, (const unsigned char*)"B: Back", FONT_LIGHT_GRAY, false);
}

void dscheevosMenu(void) {
	const DScheevosStatus* s = dscheevosStatus();
	const u32 reconnect = dscheevosReconnectEntry();
	bool reconnectAsked = false;
	bool list = false;   // false: the summary page
	bool detail = false;
	u32 sel = 0;

	keysLast = KEYS; // keys still held from the main menu don't count
	keyDelay = 20;
	while (1) {
		const DScheevosList* l = dscheevosList();
		const u32 count = l ? l->count : 0;
		if (sel >= count) {
			sel = count ? count - 1 : 0;
		}

		clearScreen(false);
		if (!s) {
			printCenter(16, 0, (const unsigned char*)"RetroAchievements", FONT_WHITE, false);
			printCenter(16, 3, (const unsigned char*)"Not active", FONT_LIGHT_GRAY, false);
			print(1, 22, (const unsigned char*)"B: Back", FONT_LIGHT_GRAY, false);
		} else if (reconnect) {
			drawStream(s, reconnectAsked);
		} else if (detail && count) {
			drawDetail(&l->e[sel]);
		} else if (list) {
			drawList(s, l, sel);
		} else {
			printCenter(16, 0, (const unsigned char*)"RetroAchievements", FONT_WHITE, false);
			drawSummary(s, reconnect, reconnectAsked);
		}

		const u32 key = getKey();
		if (key & KEY_B) {
			if (detail) {
				detail = false;
				continue;
			}
			break;
		}
		if (!s || detail) {
			continue;
		}
		if (!list) {
			if ((key & KEY_A) && reconnect && !reconnectAsked) {
				// the module only takes note: the work runs once the game does
				(*(void (*)(void))reconnect)();
				reconnectAsked = true;
			} else if ((key & (KEY_RIGHT | KEY_DOWN)) && !reconnect) {   // Cheevotron: no list here
				list = true;
			}
			continue;
		}
		if (key & KEY_A) {
			detail = (count > 0);
		} else if (key & KEY_UP) {
			if (sel > 0) sel--;
		} else if (key & KEY_DOWN) {
			if (sel + 1 < count) sel++;
		} else if (key & KEY_LEFT) {
			if (sel < LIST_ROWS) {
				list = false;
			} else {
				sel = (sel / LIST_ROWS - 1) * LIST_ROWS;
			}
		} else if (key & KEY_RIGHT) {
			if ((sel / LIST_ROWS + 1) * LIST_ROWS < count) {
				sel = (sel / LIST_ROWS + 1) * LIST_ROWS;
			}
		}
	}

	do { waitFrame(); } while (KEYS & KEY_B);
}
#endif
