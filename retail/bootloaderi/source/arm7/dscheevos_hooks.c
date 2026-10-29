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
#include <nds/timers.h>
#include "tonccpy.h"
#include "my_fat.h"
#include "debug_file.h"
#include "nds_header.h"
#include "cardengine_header_arm7.h"
#include "cardengine_header_arm9.h"
#include "common.h"
#include "value_bits.h"
#include "dscheevos_hooks.h"
#include "dscm_record.h"
#include "locations.h"
#include "igm_text.h"
#include "cheevotron_loading.h"
#include "dscheevos_cardboot.h"

extern void readFirmware(u32 address, void* destination, u32 size); // libnds/firmware.c
extern u32 FAT_NextCluster(u32 cluster, const bool boolCard2);     // my_fat.c
extern int discBytePerClus[2];
extern u32 dscheevosModuleCluster;
extern u32 dscheevosSessionCluster;
extern u32 dscheevosPendingCluster;
extern u32 dscheevosRecordCluster;
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

// The session's flags byte (DSCHEEVOS_BOOT_*), read once.
static u8 dscheevosFlags(void) {
	static int cached = -1;
	if (cached < 0) {
		u8 flags = 0;
		if (dscheevosWanted()) {
			aFile file;
			getFileFromCluster(&file, dscheevosSessionCluster, false);
			fileRead((char*)&flags, &file, DSCHEEVOS_S_FLAGS, 1);
		}
		cached = flags;
	}
	return (u8)cached;
}

// A Cheevotron stream session: the console runs no RetroAchievements itself,
// so there is no achievement placard to show before the game.
bool dscheevosStream(void) {
	return (dscheevosFlags() & DSCHEEVOS_BOOT_STREAM) != 0;
}

// Boot the inserted Slot-1 cartridge natively instead of the SD ROM.
bool dscheevosBootFromCart(void) {
	return (dscheevosFlags() & DSCHEEVOS_BOOT_CART) != 0;
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

// The module's status, at its physical address: the module's default build
// links its data through the 0x0C000000 mirror, and the ARM7 has no cache
// and reads the RAM where it is. NULL if the header's pointer is off.
static const DScheevosStatus* statusPhys(void) {
	const DScheevosStatus* s = *(const DScheevosStatus**)(DSCHEEVOS_LOCATION + DSCHEEVOS_H_STATUS);
	s = (const DScheevosStatus*)(((u32)s & 0x00FFFFFF) | 0x02000000);
	if ((u32)s < DSCHEEVOS_LOCATION || (u32)s + sizeof(DScheevosStatus) > DSCHEEVOS_LOCATION + DSCHEEVOS_SIZE) {
		return NULL;
	}
	return s;
}

static u32 bootTicks(void) {
	return ((u32)TIMER_DATA(1) << 16) | TIMER_DATA(0);
}

// Wait for the ARM9 to finish the module's init. After a minute, log what the
// module's status says (its active and error words go through the uncached
// mirror, so they are in RAM at once): active set means the init finished and
// the ARM9 hung on its way back; nothing set means it hung inside. Then keep
// waiting, as before.
static void waitArm9Init(void) {
	const u32 start = bootTicks();
	bool reported = false;
	while (arm9_stateFlag != ARM9_READY) {
		if (!reported && bootTicks() - start > 60u * 32768u) {
			const DScheevosStatus* s = statusPhys();
			reported = true;
			dbg_printf("DScheevos: ARM9 init over 60 s, status active=");
			dbg_hexa(s ? s->active : 0xFFFFFFFF);
			dbg_printf(" error=");
			dbg_hexa(s ? s->error : 0xFFFFFFFF);
			dbg_printf("\n");
		}
	}
}

static void placard(void) {
	const DScheevosStatus* s = statusPhys();
	if (!s || !s->active || s->error) {
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

// The Cheevotron loading screen (stream sessions, cheevotron_loading.h), drawn
// here like the session card above into the ESRB screen's buffer (a stream
// session shows no ESRB screen). The ARM9 shows it and keeps its dots moving
// (main.arm9.c). The first stage has shown the same screen since its start.
#define LOADING_SCREEN    ((u8*)IMAGES_LOCATION)

void dscheevosLoadingScreen(void) {
	if (!dscheevosStream()) {
		return;
	}
	char path[256];
	aFile file;
	getFileFromCluster(&file, dscheevosSessionCluster, false);
	fileRead(path, &file, DSCHEEVOS_S_ROM_PATH, sizeof(path));
	path[sizeof(path) - 1] = 0;

	toncset(LOADING_SCREEN, CHEEVOTRON_LOADING_BACKGROUND, 256 * 192);
	cheevotronLoadingScreen(LOADING_SCREEN, path);
	arm9_loadingScreenReady = true;
}

// The page file's copy of the game's binaries (for soft resets and the
// in-game menu) is the same at every start of the same game, and writing it
// took 1.6 s. Reading is much faster than writing on the SD card: compare it
// chunk by chunk and write only the chunks that differ. Stream sessions only:
// the scratch is the ESRB screen's buffer after the loading screen, and a
// stream session shows no ESRB screen.
#define PAGE_SCRATCH  ((u8*)IMAGES_LOCATION + 256 * 192)
#define PAGE_CHUNK    0xC000

void dscheevosPageFileUpdate(const char* src, aFile* file, u32 offset, u32 size) {
	u32 written = 0;
	for (u32 done = 0; done < size; done += PAGE_CHUNK) {
		const u32 n = (size - done < PAGE_CHUNK) ? size - done : PAGE_CHUNK;
		fileRead((char*)PAGE_SCRATCH, file, offset + done, n);
		if (memcmp(PAGE_SCRATCH, src + done, n) != 0) {
			fileWrite(src + done, file, offset + done, n);
			written += n;
		}
	}
	toncset(PAGE_SCRATCH, 0, PAGE_CHUNK);
	dbg_printf("DScheevos: page file ");
	dbg_hexa(written);
	dbg_printf(" of ");
	dbg_hexa(size);
	dbg_printf(" bytes written\n");
}

// Boot clock for the log: milliseconds since the bootloader started (ARM7
// timers 0 and 1 cascaded at 32768 Hz, main.arm7.c). One line, one log write.
void dscheevosStamp(const char* what) {
	static const char head[] = "DScheevos t=";
	u32 ms = (bootTicks() * 125) >> 12;	// 32768 Hz ticks
	char digits[12];
	char out[96];
	int n = 0, m = 0;
	for (int i = 0; head[i]; i++) {
		out[m++] = head[i];
	}
	do {
		digits[n++] = '0' + ms % 10;
		ms /= 10;
	} while (ms);
	while (n) {
		out[m++] = digits[--n];
	}
	out[m++] = ' ';
	out[m++] = 'm';
	out[m++] = 's';
	out[m++] = ':';
	out[m++] = ' ';
	for (int i = 0; what[i] && m < (int)sizeof(out) - 2; i++) {
		out[m++] = what[i];
	}
	out[m++] = '\n';
	out[m] = 0;
	dbg_printf(out);
}

static void disable(const char* why) {
	*(vu32*)(DSCHEEVOS_LOCATION + DSCHEEVOS_H_MAGIC) = 0;
	dbg_printf("DScheevos off: ");
	dbg_printf(why);
	dbg_printf("\n");
}

// Cheevotron stream sessions: claim a slot of recording.bin for this run, for
// the card engine's copy of the streamer's recording (dscm_record.h,
// cardenginei/arm7 dscheevos_record.c). The file must be in one piece on the
// card: the card engine writes its sectors by number, with no FAT in sight.
// Takes the slot that holds no recording the phone still wants, writes its
// header as the claim (the next seq, the game, no data yet) and tells the
// card engine where it is. Without a file, or a fragmented one, play goes on
// with no copy (the ring in RAM still serves a replay during the run).
static void dscheevosRecordSetup(cardengineArm7* ce7) {
	static u32 sector[DSCM_REC_SECTOR_BYTES / 4];
	ce7->dscheevosRecordSector = 0;
	if (!dscheevosStream() || !dscheevosRecordCluster) {
		return;
	}
	aFile file;
	getFileFromCluster(&file, dscheevosRecordCluster, false);
	fileRead((char*)sector, &file, 0, DSCM_REC_SECTOR_BYTES);
	const dscm_recfile_t* f = (const dscm_recfile_t*)sector;
	if (f->magic != DSCM_RECFILE_MAGIC || f->version != DSCM_RECFILE_VERSION
	 || f->data_sectors != DSCM_REC_DATA_SECTORS || f->slots != DSCM_REC_SLOTS) {
		dbg_printf("DScheevos: recording.bin isn't the layout this knows, no copy\n");
		return;
	}
	char code[4], title[12], hash[32];
	tonccpy(code, f->game_code, sizeof(code));
	tonccpy(title, f->title, sizeof(title));
	tonccpy(hash, f->game_hash, sizeof(hash));

	const u32 bytesPerClus = (u32)discBytePerClus[0];
	const u32 clusters = (DSCM_REC_FILE_SIZE + bytesPerClus - 1) / bytesPerClus;
	u32 c = dscheevosRecordCluster;
	for (u32 i = 1; i < clusters; i++) {
		const u32 n = FAT_NextCluster(c, false);
		if (n != c + 1) {
			dbg_printf("DScheevos: recording.bin is fragmented, no copy\n");
			return;
		}
		c = n;
	}

	dscm_recslot_t s[DSCM_REC_SLOTS];
	for (u32 i = 0; i < DSCM_REC_SLOTS; i++) {
		fileRead((char*)&s[i], &file, DSCM_REC_SLOT_SECTOR(i) * DSCM_REC_SECTOR_BYTES, sizeof(s[i]));
	}
	const int slot = dscm_rec_pick_slot(&s[0], &s[1]);
	const u32 seq = dscm_rec_next_seq(&s[0], &s[1]);

	toncset(sector, 0, sizeof(sector));
	dscm_recslot_t* h = (dscm_recslot_t*)sector;
	h->magic = DSCM_RECSLOT_MAGIC;
	h->size = sizeof(*h);
	h->seq = seq;
	h->flags = DSCM_RECSLOT_CLAIMED;
	tonccpy(h->game_code, code, sizeof(h->game_code));
	tonccpy(h->title, title, sizeof(h->title));
	tonccpy(h->game_hash, hash, sizeof(h->game_hash));
	h->data_sectors = DSCM_REC_DATA_SECTORS;
	fileWrite((const char*)sector, &file, DSCM_REC_SLOT_SECTOR(slot) * DSCM_REC_SECTOR_BYTES, DSCM_REC_SECTOR_BYTES);

	ce7->dscheevosRecordDataSectors = DSCM_REC_DATA_SECTORS;
	ce7->dscheevosRecordSeq = seq;
	tonccpy(ce7->dscheevosRecordTitle, title, sizeof(ce7->dscheevosRecordTitle));
	tonccpy(ce7->dscheevosRecordCode, code, sizeof(ce7->dscheevosRecordCode));
	ce7->dscheevosRecordSector = FAT_ClustToSect(dscheevosRecordCluster, false) + DSCM_REC_SLOT_SECTOR(slot);
	dbg_printf("DScheevos: recording slot ");
	dbg_hexa(slot);
	dbg_printf(" claimed, seq ");
	dbg_hexa(seq);
	dbg_printf(", header sector ");
	dbg_hexa(ce7->dscheevosRecordSector);
	dbg_printf(dscm_recslot_holds(&s[slot ^ 1]) ? ", the other slot holds a run the phone hasn't replayed\n" : "\n");
}

// session.used keeps its clusters; once the session is in RAM its contents
// are zeroed, so the file can't be booted again (and the login token in it
// doesn't stay on the SD card).
static void wipeSession(aFile* file, const u32 size) {
	static u32 zero[128];
	dscheevosNetWanted(); // both read the file once and keep the answer
	dscheevosHardcore();
	dscheevosStamp("session flags read");
	for (u32 i = 0; i < size; i += sizeof(zero)) {
		fileWrite((char*)zero, file, i, (size - i < sizeof(zero)) ? size - i : sizeof(zero));
	}
}

void dscheevosSetup(cardengineArm7* ce7, cardengineArm9* ce9, const tNDSHeader* ndsHeader) {
	if (!dscheevosWanted()) {
		return;
	}

	dscheevosStamp("setup start");
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
	dscheevosStamp("module read");

	getFileFromCluster(&file, dscheevosSessionCluster, false);
	u32* session = (u32*)DSCHEEVOS_SESSION;
	fileRead((char*)session, &file, 0, 0x10);
	const u32 sessionSize = session[DSCHEEVOS_S_FILE_SIZE / 4];
	if (session[0] != DSCHEEVOS_SESSION_MAGIC || sessionSize < 0x10 || sessionSize > DSCHEEVOS_SESSION_MAX) {
		disable("bad session");
		return;
	}
	fileRead((char*)session, &file, 0, sessionSize);
	dscheevosStamp("session read");
	wipeSession(&file, sessionSize);
	dscheevosStamp("session wiped");
	if (session[DSCHEEVOS_S_SIZE / 4] != DSCHEEVOS_S_BOOT_SIZE || sessionSize < DSCHEEVOS_S_BOOT_SIZE) {
		disable("old session");
		return;
	}
	fillPmk((u8*)session);
	// the speed the player chose before launch, for the module's status and journal
	((u8*)session)[DSCHEEVOS_S_CLOCK] = (boostCpu ? DSCHEEVOS_CLOCK_CPU_133 : 0) | (boostVram ? DSCHEEVOS_CLOCK_VRAM : 0);
	// Cart mode: the launcher can't hash a cartridge (KEY1 is the bootloader's),
	// so the session comes with no hash and loadBinary_ARM7_cart computed it
	// from the card; the module's HELLO carries it from here.
	if (dscheevosBootFromCart()) {
		tonccpy((u8*)session + DSCHEEVOS_S_GAME_HASH, DSCM_CART_HASH, 33);
		dbg_printf("DScheevos cart: session hash ");
		dbg_printf((const char*)session + DSCHEEVOS_S_GAME_HASH);
		dbg_printf("\n");
	}

	if (patchOsHalt(ndsHeader, header[DSCHEEVOS_H_HALT / 4]) == 0) {
		disable("no OS_Halt");
		return;
	}
	dscheevosStamp("OS_Halt patched");

	header[DSCHEEVOS_H_VBLANK_COUNT / 4] = 0;
	ce7->dscheevosPendingCluster = dscheevosPendingCluster;
	dscheevosRecordSetup(ce7);
	dscheevosStamp("recording slot claimed");
	ce7->valueBits |= DSCHEEVOS_VALUEBIT;
	ce9->valueBits |= DSCHEEVOS_VALUEBIT;

	while (arm9_stateFlag != ARM9_READY); // an ESRB screen may still be up
	dscheevosStamp("ARM9 init start");
	arm9_stateFlag = ARM9_DSCHEEVOS_INIT;
	waitArm9Init();
	dbg_printf("DScheevos: module ready\n");
	dscheevosStamp("module ready");
	if (!dscheevosStream()) {   // a Cheevotron stream session shows no placard
		placard();
	}
	dscheevosStamp("placard done");
	if (!dscheevosPendingCluster) {
		dbg_printf("DScheevos: no pending.bin, unsent unlocks won't be kept\n");
	}
}
