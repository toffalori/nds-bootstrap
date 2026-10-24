/*
 * DScheevos (RetroAchievements) hook points in the DSi-mode bootloader:
 * load the module and the session file into the RAM the ROM cache gave up,
 * point the game's OS_Halt at the module, and switch on the card engine
 * hooks (ARM7 VBlank count + IPC sync, ARM9 IPC handler call). The module
 * is initialized by the bootloader's ARM9 before the game starts.
 *
 * Only DS (NTR-mode) games booted from SD use this.
 */
#include <stddef.h>
#include <string.h>
#include <nds/ndstypes.h>
#include <nds/memory.h>
#include "tonccpy.h"
#include "my_fat.h"
#include "debug_file.h"
#include "nds_header.h"
#include "cardengine_header_arm7.h"
#include "cardengine_header_arm9.h"
#include "common.h"
#include "value_bits.h"
#include "dscheevos_hooks.h"
#include "locations.h"
#include "igm_text.h"

extern void readFirmware(u32 address, void* destination, u32 size); // libnds/firmware.c
extern u32 dscheevosModuleCluster;
extern u32 dscheevosSessionCluster;
extern u32 dscheevosPendingCluster;
extern u32 iUncompressedSize;
extern u32 pageFileCluster;
extern u8 bootstrapOnFlashcard;

bool dscheevosWanted(void) {
	return dscheevosModuleCluster && dscheevosSessionCluster && !dsiModeConfirmed && !isDSiWare;
}

// The session keeps the launcher's Wi-Fi connection during play: the DSi
// Wi-Fi chip must stay selected (no DS-mode Wi-Fi at boot).
bool dscheevosNetWanted(void) {
	static int cached = -1;
	if (cached < 0) {
		u8 valid = 0;
		if (dscheevosWanted()) {
			aFile file;
			getFileFromCluster(&file, dscheevosSessionCluster, false);
			fileRead((char*)&valid, &file, DSCHEEVOS_S_NET_VALID, 1);
		}
		cached = valid ? 1 : 0;
	}
	return cached;
}

// A hardcore session: the bootloader trusts nothing on the SD card that
// changes the game (see the patch offset cache in main.arm7.c).
bool dscheevosHardcore(void) {
	static int cached = -1;
	if (cached < 0) {
		u8 hardcore = 0;
		if (dscheevosWanted()) {
			aFile file;
			getFileFromCluster(&file, dscheevosSessionCluster, false);
			fileRead((char*)&hardcore, &file, DSCHEEVOS_S_HARDCORE, 1);
		}
		cached = hardcore ? 1 : 0;
	}
	return cached;
}

// NitroSDK OS_Halt: mov r0, #0; mcr p15, 0, r0, c7, c0, 4; bx lr
static int patchOsHalt(const tNDSHeader* ndsHeader, u32 haltEntry) {
	u32* p = (u32*)ndsHeader->arm9destination;
	u32* end = p + (iUncompressedSize ? iUncompressedSize : ndsHeader->arm9binarySize) / 4 - 2;
	int count = 0;
	for (; p < end; p++) {
		if (p[0] == 0xE3A00000 && p[1] == 0xEE070F90 && p[2] == 0xE12FFF1E) {
			p[0] = 0xE51FF004; // ldr pc, [pc, #-4]
			p[1] = haltEntry;
			dbg_printf("DScheevos: OS_Halt at ");
			dbg_hexa((u32)p);
			dbg_printf("\n");
			count++;
		}
	}
	return count;
}

static u16 crc16(const u8* data, int length) {
	u32 crc = 0;
	for (int i = 0; i < length; i++) {
		crc ^= data[i];
		for (int j = 0; j < 8; j++) {
			crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
		}
	}
	return crc;
}

// WPA2 rekeys during play need the access point's PMK. The console keeps it
// with its own saved Wi-Fi settings (DSi slots 4-6 in the firmware flash):
// read it, read-only, into the session in RAM. It never goes to the SD card.
static void fillPmk(u8* session) {
	session[DSCHEEVOS_S_PMK_VALID] = 0;
	toncset(session + DSCHEEVOS_S_PMK, 0, 32);
	const u32 ssidLen = session[DSCHEEVOS_S_AP_SSID_LEN];
	if (!session[DSCHEEVOS_S_NET_VALID] || session[DSCHEEVOS_S_AP_SECURITY] != DSCHEEVOS_AP_WPA2
	 || ssidLen == 0 || ssidLen > 32) {
		return;
	}

	u16 settingsOffset = 0;
	readFirmware(0x20, &settingsOffset, sizeof(settingsOffset));
	const u32 base = settingsOffset * 8 - 0xA00;
	u8* slot = (u8*)(DSCHEEVOS_SESSION + DSCHEEVOS_SESSION_MAX); // module heap, not in use yet
	for (int i = 0; i < 3; i++) {
		readFirmware(base + i * 0x200, slot, 0x200);
		if (crc16(slot, 0x100) != 0 || crc16(slot + 0x100, 0x100) != 0
		 || slot[0xEF] == 0       // slot unused
		 || slot[0xE7] == 0       // not WPA
		 || memcmp(slot + 0x40, session + DSCHEEVOS_S_AP_SSID, ssidLen) != 0
		 || (ssidLen < 32 && slot[0x40 + ssidLen] != 0)) {
			continue;
		}
		tonccpy(session + DSCHEEVOS_S_PMK, slot + 0x100, 32);
		session[DSCHEEVOS_S_PMK_VALID] = 1;
		dbg_printf("DScheevos: PMK from Wi-Fi slot ");
		dbg_hexa(4 + i);
		dbg_printf("\n");
		break;
	}
	toncset(slot, 0, 0x200);
}

// The session card shown before the game starts (ARM9_DSCHEEVOS_PLACARD):
// drawn here into the ESRB screen's buffer, with the in-game menu's font,
// then shown by the ARM9 on the top screen until the screen is cleared.
#define PLACARD ((u16*)IMAGES_LOCATION)
#define PLACARD_BG     (BIT(15) | 0x1862)
#define PLACARD_GOLD   (BIT(15) | 0x131F)
#define PLACARD_WHITE  0xFFFF
#define PLACARD_GRAY   (BIT(15) | 0x5294)
#define PLACARD_RED    (BIT(15) | 0x18DF)
#define PLACARD_BLUE   (BIT(15) | 0x7F10)
#define PLACARD_YELLOW (BIT(15) | 0x2BFF)
static u8 placardFont[128 * 8]; // the font's ASCII half: 8x8, 1 bit per pixel

// One centered line of at most 32 characters. As in the in-game menu's
// fit(), UTF-8 and other non-ASCII characters become '?'
static void placardText(int row, const char* in, u16 color) {
	u8 line[32];
	int n = 0;
	while (*in && n < 32) {
		const u8 c = (u8)*in++;
		if (c >= 0x80 && c < 0xC0) {
			continue; // UTF-8 continuation byte
		}
		line[n++] = (c >= 0x20 && c < 0x7F) ? c : '?';
	}
	u16* dst = PLACARD + row * 8 * 256 + (256 - n * 8) / 2;
	for (int i = 0; i < n; i++, dst += 8) {
		const u8* glyph = placardFont + line[i] * 8;
		for (int y = 0; y < 8; y++) {
			for (int x = 0; x < 8; x++) {
				if (glyph[y] & BIT(x)) {
					dst[y * 256 + x] = color;
				}
			}
		}
	}
}

static char* placardNum(char* p, u32 val) {
	char digits[10];
	int n = 0;
	do {
		digits[n++] = '0' + val % 10;
		val /= 10;
	} while (val);
	while (n) {
		*p++ = digits[--n];
	}
	return p;
}

static char* placardStr(char* p, const char* s) {
	while (*s) {
		*p++ = *s++;
	}
	return p;
}

static void placard(void) {
	const DScheevosStatus* s = *(const DScheevosStatus**)(DSCHEEVOS_LOCATION + DSCHEEVOS_H_STATUS);
	// The module's default build links its data through the 0x0C000000 mirror;
	// the ARM7 has no cache and reads the RAM at its physical address.
	s = (const DScheevosStatus*)(((u32)s & 0x00FFFFFF) | 0x02000000);
	if ((u32)s < DSCHEEVOS_LOCATION || (u32)s + sizeof(DScheevosStatus) > DSCHEEVOS_LOCATION + DSCHEEVOS_SIZE
	 || !s->active || s->error) {
		return;
	}
	aFile file;
	getFileFromCluster(&file, pageFileCluster, bootstrapOnFlashcard);
	fileRead((char*)placardFont, &file, offsetof(struct IgmText, font), sizeof(placardFont));

	toncset16(PLACARD, PLACARD_BG, 256*192);
	placardText(6, "RetroAchievements", PLACARD_GOLD);
	placardText(9, s->gameTitle, PLACARD_WHITE);
	placardText(12, s->hardcore ? "HARDCORE" : "Casual", s->hardcore ? PLACARD_RED : PLACARD_BLUE);
	if (s->hardcore == DSCHEEVOS_HARDCORE_REFUSED) {
		placardText(13, "Rules on, RA counts as Casual", PLACARD_GRAY);
	}
	char line[48];
	char* p = placardNum(line, s->achievementsUnlocked);
	*p++ = '/';
	p = placardStr(placardNum(p, s->achievements), " achievements, ");
	p = placardNum(p, s->pointsUnlocked);
	*p++ = '/';
	p = placardStr(placardNum(p, s->points), " points");
	*p = 0;
	placardText(15, line, PLACARD_WHITE);
	if (*(u8*)(DSCHEEVOS_SESSION + DSCHEEVOS_S_FLAGS) & DSCHEEVOS_BOOT_SPECTATOR) {
		placardText(17, "Spectator: nothing is sent", PLACARD_YELLOW);
	}

	arm9_stateFlag = ARM9_DSCHEEVOS_PLACARD;
	while (arm9_stateFlag != ARM9_READY);
}

static void disable(const char* why) {
	*(vu32*)(DSCHEEVOS_LOCATION + DSCHEEVOS_H_MAGIC) = 0;
	dbg_printf("DScheevos off: ");
	dbg_printf(why);
	dbg_printf("\n");
}

// session.used keeps its clusters; once the session is in RAM its contents
// are zeroed, so the file can't be booted again (and the login token in it
// doesn't stay on the SD card).
static void wipeSession(aFile* file, const u32 size) {
	static u32 zero[128];
	dscheevosNetWanted(); // both read the file once and keep the answer
	dscheevosHardcore();
	for (u32 i = 0; i < size; i += sizeof(zero)) {
		fileWrite((char*)zero, file, i, (size - i < sizeof(zero)) ? size - i : sizeof(zero));
	}
}

void dscheevosSetup(cardengineArm7* ce7, cardengineArm9* ce9, const tNDSHeader* ndsHeader) {
	if (!dscheevosWanted()) {
		return;
	}

	aFile file;
	getFileFromCluster(&file, dscheevosModuleCluster, false);
	u32* header = (u32*)DSCHEEVOS_LOCATION;
	fileRead((char*)header, &file, 0, 0x28);
	const u32 imageSize = header[DSCHEEVOS_H_IMAGE_SIZE / 4];
	if (header[DSCHEEVOS_H_MAGIC / 4] != DSCHEEVOS_MAGIC || header[DSCHEEVOS_H_VERSION / 4] != DSCHEEVOS_VERSION
	 || imageSize < 0x28 || imageSize > DSCHEEVOS_IMAGE_MAX) {
		disable("bad module");
		return;
	}
	fileRead((char*)header, &file, 0, imageSize);

	getFileFromCluster(&file, dscheevosSessionCluster, false);
	u32* session = (u32*)DSCHEEVOS_SESSION;
	fileRead((char*)session, &file, 0, 0x10);
	const u32 sessionSize = session[DSCHEEVOS_S_FILE_SIZE / 4];
	if (session[0] != DSCHEEVOS_SESSION_MAGIC || sessionSize < 0x10 || sessionSize > DSCHEEVOS_SESSION_MAX) {
		disable("bad session");
		return;
	}
	fileRead((char*)session, &file, 0, sessionSize);
	wipeSession(&file, sessionSize);
	if (session[DSCHEEVOS_S_SIZE / 4] != DSCHEEVOS_S_BOOT_SIZE || sessionSize < DSCHEEVOS_S_BOOT_SIZE) {
		disable("old session");
		return;
	}
	fillPmk((u8*)session);
	// the speed the player chose before launch, for the module's status and journal
	((u8*)session)[DSCHEEVOS_S_CLOCK] = (boostCpu ? DSCHEEVOS_CLOCK_CPU_133 : 0) | (boostVram ? DSCHEEVOS_CLOCK_VRAM : 0);

	if (patchOsHalt(ndsHeader, header[DSCHEEVOS_H_HALT / 4]) == 0) {
		disable("no OS_Halt");
		return;
	}

	header[DSCHEEVOS_H_VBLANK_COUNT / 4] = 0;
	ce7->dscheevosPendingCluster = dscheevosPendingCluster;
	ce7->valueBits |= DSCHEEVOS_VALUEBIT;
	ce9->valueBits |= DSCHEEVOS_VALUEBIT;

	while (arm9_stateFlag != ARM9_READY); // an ESRB screen may still be up
	arm9_stateFlag = ARM9_DSCHEEVOS_INIT;
	while (arm9_stateFlag != ARM9_READY);
	dbg_printf("DScheevos: module ready\n");
	placard();
	if (!dscheevosPendingCluster) {
		dbg_printf("DScheevos: no pending.bin, unsent unlocks won't be kept\n");
	}
}
