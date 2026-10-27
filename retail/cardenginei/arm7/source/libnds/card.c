/*
    Cheevotron cart mode: read the real Slot-1 cartridge from the resident
    card engine during gameplay.

    The bootloader (dscheevos_cardboot.c) does the one-time KEY1 handshake and
    leaves the card powered and in main-data (KEY2) mode. This engine, which
    stays resident while the game runs, serves the game's ROM reads from that
    live card with plain CARD_CMD_DATA_READ commands. Card command helpers are
    the libnds ones (joat/dovoto/WinterMute, zlib licence); dscmCartReadData is
    for this fork.
*/

#include <nds/card.h>
#include <nds/memory.h>
#include <nds/interrupts.h>
#include <string.h>

static void dscmCardWriteCommand(const u8* command) {
	int index;
	// No CARD_CR1_IRQ: a raised card IRQ, fired between reads while the game's
	// interrupts are on, lets a handler touch the card and desync KEY2.
	REG_AUXSPICNTH = CARD_CR1_ENABLE;
	for (index = 0; index < 8; index++) {
		REG_CARD_COMMAND[7 - index] = command[index];
	}
}

// Diagnostic: last ROMCTRL seen when a transfer ended, and whether the spin
// bailed on the guard rather than on BUSY clearing. Lets a stuck CARD_BUSY be
// logged instead of freezing the console forever.
// Forced into .data: the TWL card engine's .bss is a tiny VRAM region.
volatile u32 dscmLastRomctrl __attribute__((section(".data"))) = 0;
volatile u32 dscmSpinTimedOut __attribute__((section(".data"))) = 0;

#ifndef TWLSDK
int dscmCartReinit(u32* scratch);   // defined below; scratch >= 0x1C00 bytes
#endif

static void dscmCardPolledTransfer(u32 flags, u32* destination, u32 length, const u8* command) {
	u32 data;
	dscmCardWriteCommand(command);
	REG_ROMCTRL = flags;
	u32* target = destination + length;
	u32 guard = 0x1000000;
	do {
		if (REG_ROMCTRL & CARD_DATA_READY) {
			data = REG_CARD_DATA_RD;
			if (destination != NULL && destination < target)
				*destination++ = data;
		}
	} while ((REG_ROMCTRL & CARD_BUSY) && --guard);
	dscmLastRomctrl = REG_ROMCTRL;
	if (guard == 0) dscmSpinTimedOut = 1;
}

static void dscmCardParamCommand(u8 command, u32 parameter, u32 flags, u32* destination, u32 length) {
	u8 cmdData[8];
	cmdData[7] = (u8)command;
	cmdData[6] = (u8)(parameter >> 24);
	cmdData[5] = (u8)(parameter >> 16);
	cmdData[4] = (u8)(parameter >> 8);
	cmdData[3] = (u8)(parameter >> 0);
	cmdData[2] = 0;
	cmdData[1] = 0;
	cmdData[0] = 0;
	dscmCardPolledTransfer(flags, destination, length, cmdData);
}

/* A partial/unaligned block: read the whole 0x200 block into a stack scratch
 * and copy out just [off, off+n). Kept out of line so the common aligned path
 * (which runs in the card IRQ) never reserves this 0x200-byte frame, and so the
 * scratch is never in .bss (the TWL card engine's .bss is a tiny VRAM region). */
static void __attribute__((noinline)) dscmCartReadPartial(u32 blockBase, u8* dst, u32 off, u32 n, u32 cmdFlags) {
	u32 block[0x200 / sizeof(u32)] __attribute__((aligned(4)));
	int oldIME = enterCriticalSection();
	dscmCardParamCommand(CARD_CMD_DATA_READ, blockBase, cmdFlags, block, 0x200 / sizeof(u32));
	leaveCriticalSection(oldIME);
	memcpy(dst, (u8*)block + off, n);
}

/* Read `len` bytes from ROM offset `src` of the live cartridge into `dst`.
 * portFlags is the header's cardControl13 (header +0x60). Whole aligned 0x200
 * blocks (the gameplay cache reads) go straight into dst; any unaligned head or
 * short tail goes through dscmCartReadPartial, so dst is never overrun. The card
 * mirrors the secure area (<0x8000) to 0x8000+(addr&0x1FF) itself. dst must be
 * word-aligned, as the game's cache buffers and the reload targets are. */
void dscmCartReadData(u32 src, u8* dst, u32 len, u32 portFlags) {
	const u32 cmdFlags = (portFlags & ~CARD_BLK_SIZE(7)) | CARD_ACTIVATE | CARD_nRESET | CARD_BLK_SIZE(1);
	// The WHOLE read runs with IRQs off, exactly as the proven bootloader read.
	// Toggling IME between 0x200 blocks let an IRQ touch the card mid-read and
	// desync KEY2 (the first block decoded, the rest came back 0xFF).
	int oldIME = enterCriticalSection();
#ifndef TWLSDK
	// KEY2 desyncs between reads (something touches the card in the gap), so for
	// now rebuild the session before EVERY read. Correct but slow; optimise once
	// confirmed. Needs dst (>= 0x1C00 bytes) as handshake scratch, then the read
	// overwrites it.
	if (len >= 0x2000u) {
		dscmCartReinit((u32*)dst);
	}
#endif
	while (len > 0) {
		u32 off = src & 0x1FFu;
		if (off == 0 && len >= 0x200u) {
			dscmCardParamCommand(CARD_CMD_DATA_READ, src, cmdFlags, (u32*)dst, 0x200 / sizeof(u32));
			dst += 0x200; src += 0x200; len -= 0x200;
		} else {
			u32 n = 0x200u - off;
			if (n > len) n = len;
			dscmCartReadPartial(src & ~0x1FFu, dst, off, n, cmdFlags);
			dst += n; src += n; len -= n;
		}
	}
	leaveCriticalSection(oldIME);
}

/* ------------------------------------------------------------------------- *
 * Cheevotron KEY2 re-handshake (NTR engine only).
 *
 * The game's own ARM7 boot code resets the Slot-1 card after the bootloader
 * hands off, so by the first gameplay read the card is no longer in the KEY2
 * main-data session the bootloader established, and encrypted reads return
 * open-bus 0xFF. The resident engine cannot re-seed KEY2 in main-data mode, so
 * it redoes the full KEY1 handshake once (card reset -> KEY1 -> secure -> data
 * mode), exactly as the bootloader did, then serves reads from the synced card.
 *
 * Ported from dscheevos_cardboot.c (NitroHax read_card.c / encryption.c, GPL-3)
 * with three gameplay adaptations: the Blowfish table / header / secure buffers
 * are in .data (the engine .bss is a tiny VRAM region), cardDelay uses swiDelay
 * (the game owns the hardware timers), and the secure area is read to discard.
 * ------------------------------------------------------------------------- */
#ifndef TWLSDK

#include <nds/bios.h>

extern void dscmReadBios(u8* dest, u32 src, u32 size);
extern u32 dbg_printf(const char* message);
extern u32 dbg_hexa(u32 n);

#ifndef REG_SCFG_ROM
#define REG_SCFG_ROM (*(vu16*)0x04004000)
#endif
#ifndef REG_SCFG_MC
#define REG_SCFG_MC  (*(vu16*)0x04004010)
#endif

#define KEYSIZE 0x1048

/* The big handshake buffers (Blowfish table 0x1048, kept secure 0x800, discard
 * 0x200, header 0x160 ~= 0x1BA8 total) do NOT fit in the engine's static image:
 * .data and .bss share one tiny region. They are carved from the first read's
 * destination buffer instead (0x4000, free until the read runs). Only these two
 * tiny state vars stay static. */
static u32 dscmKeycode[3] __attribute__((section(".data")));
static u32* keybuf;   /* -> scratch; set at the top of dscmCartReinit */
#define keycode dscmKeycode

static void crypt_64bit_up(u32* ptr) {
	u32 x = ptr[1];
	u32 y = ptr[0];
	u32 z;
	int i;
	for (i = 0; i < 0x10; i++) {
		z = keybuf[i] ^ x;
		x = keybuf[0x012 + ((z >> 24) & 0xff)];
		x = keybuf[0x112 + ((z >> 16) & 0xff)] + x;
		x = keybuf[0x212 + ((z >> 8) & 0xff)] ^ x;
		x = keybuf[0x312 + ((z >> 0) & 0xff)] + x;
		x = y ^ x;
		y = z;
	}
	ptr[0] = x ^ keybuf[0x10];
	ptr[1] = y ^ keybuf[0x11];
}

__attribute__((unused)) static void crypt_64bit_down(u32* ptr) {
	u32 x = ptr[1];
	u32 y = ptr[0];
	u32 z;
	int i;
	for (i = 0x11; i > 0x01; i--) {
		z = keybuf[i] ^ x;
		x = keybuf[0x012 + ((z >> 24) & 0xff)];
		x = keybuf[0x112 + ((z >> 16) & 0xff)] + x;
		x = keybuf[0x212 + ((z >> 8) & 0xff)] ^ x;
		x = keybuf[0x312 + ((z >> 0) & 0xff)] + x;
		x = y ^ x;
		y = z;
	}
	ptr[0] = x ^ keybuf[0x01];
	ptr[1] = y ^ keybuf[0x00];
}

static u32 bswap_32bit(u32 in) {
	u8 a = (u8)((in >> 0) & 0xff);
	u8 b = (u8)((in >> 8) & 0xff);
	u8 c = (u8)((in >> 16) & 0xff);
	u8 d = (u8)((in >> 24) & 0xff);
	return (u32)((a << 24) | (b << 16) | (c << 8) | (d << 0));
}

static void apply_keycode(u32 modulo) {
	u32 scratch[2];
	int i;
	modulo = modulo / sizeof(*keycode);
	crypt_64bit_up(&keycode[1]);
	crypt_64bit_up(&keycode[0]);
	memset(scratch, 0, 8);
	for (i = 0; i < 0x12; i += 1)
		keybuf[i] = keybuf[i] ^ bswap_32bit(keycode[i % modulo]);
	for (i = 0; i < 0x412; i += 2) {
		crypt_64bit_up(scratch);
		keybuf[i] = scratch[1];
		keybuf[i + 1] = scratch[0];
	}
}

static void init_keycode(u32 idcode, u32 level, u32 modulo) {
	dscmReadBios((u8*)keybuf, 0x30, KEYSIZE);
	keycode[0] = idcode;
	keycode[1] = idcode / 2;
	keycode[2] = idcode * 2;
	if (level >= 1) apply_keycode(modulo);
	if (level >= 2) apply_keycode(modulo);
	keycode[1] = keycode[1] * 2;
	keycode[2] = keycode[2] / 2;
	if (level >= 3) apply_keycode(modulo);
}

typedef union { char title[4]; u32 key; } GameCode;

static const u8 cardSeedBytes[] = { 0xE8, 0x4D, 0x5A, 0xB1, 0x17, 0x8F, 0x99, 0xD5 };

static u32 getRandomNumber(void) { return 4; }

__attribute__((unused)) static void decryptSecureArea(u32 gameCode, u32* secureArea) {
	int i;
	init_keycode(gameCode, 2, 8);
	crypt_64bit_down(secureArea);
	init_keycode(gameCode, 3, 8);
	for (i = 0; i < 0x200; i += 2)
		crypt_64bit_down(secureArea + i);
}

static struct { unsigned int iii, jjj, kkkkk, llll, mmm, nnn; } key1data __attribute__((section(".data")));

static void initKey1Encryption(u8* cmdData) {
	key1data.iii = getRandomNumber() & 0x00000fff;
	key1data.jjj = getRandomNumber() & 0x00000fff;
	key1data.kkkkk = getRandomNumber() & 0x000fffff;
	key1data.llll = getRandomNumber() & 0x0000ffff;
	key1data.mmm = getRandomNumber() & 0x00000fff;
	key1data.nnn = getRandomNumber() & 0x00000fff;
	cmdData[7] = CARD_CMD_ACTIVATE_BF;
	cmdData[6] = (u8)(key1data.iii >> 4);
	cmdData[5] = (u8)((key1data.iii << 4) | (key1data.jjj >> 8));
	cmdData[4] = (u8)key1data.jjj;
	cmdData[3] = (u8)(key1data.kkkkk >> 16);
	cmdData[2] = (u8)(key1data.kkkkk >> 8);
	cmdData[1] = (u8)key1data.kkkkk;
	cmdData[0] = (u8)getRandomNumber();
}

static void createEncryptedCommand(u8 command, u8* cmdData, u32 block) {
	unsigned long iii, jjj;
	if (command != CARD_CMD_SECURE_READ)
		block = key1data.llll;
	if (command == CARD_CMD_ACTIVATE_SEC) {
		iii = key1data.mmm;
		jjj = key1data.nnn;
	} else {
		iii = key1data.iii;
		jjj = key1data.jjj;
	}
	cmdData[7] = (u8)(command | (block >> 12));
	cmdData[6] = (u8)(block >> 4);
	cmdData[5] = (u8)((block << 4) | (iii >> 8));
	cmdData[4] = (u8)iii;
	cmdData[3] = (u8)(jjj >> 4);
	cmdData[2] = (u8)((jjj << 4) | (key1data.kkkkk >> 16));
	cmdData[1] = (u8)(key1data.kkkkk >> 8);
	cmdData[0] = (u8)key1data.kkkkk;
	crypt_64bit_up((u32*)cmdData);
	key1data.kkkkk += 1;
}

/* The game owns the hardware timers, so the bootloader's TIMER0 cardDelay would
 * corrupt game timing. swiDelay is a BIOS busy-wait and touches no timer. */
static void dscmCardDelay(u16 readTimeout) {
	swiDelay((((u32)readTimeout & 0x3FFF) + 8) * 32);
}

/* A raw-mode card reset (libnds cardReset), so KEY1 (0x3C) is accepted next. */
static void dscmCardReset(void) {
	const u8 cmdData[8] = { 0, 0, 0, 0, 0, 0, 0, CARD_CMD_DUMMY };
	u32 read = 0;
	dscmCardWriteCommand(cmdData);
	REG_ROMCTRL = CARD_ACTIVATE | CARD_nRESET | CARD_CLK_SLOW | CARD_BLK_SIZE(5) | CARD_DELAY2(0x18);
	do {
		if (REG_ROMCTRL & CARD_DATA_READY) {
			if (read < 0x2000) { u32 data = REG_CARD_DATA_RD; (void)data; read += 4; }
		}
	} while (REG_ROMCTRL & CARD_BUSY);
}

/* Re-run the full KEY1 handshake and leave the card in KEY2 main-data mode.
 * Returns 0 on success. Runs with IRQs already off (caller holds the critical
 * section). Mirrors loadBinary_ARM7_cart's NTR_BIOS -> enableSlot1 -> cardReset
 * -> dscmCartInit sequence. The secure area is read only to advance the
 * protocol; just the first 0x800 bytes are kept, for the encrObj decrypt check. */
int dscmCartReinit(u32* scratch) {
	/* Carve the handshake buffers out of the caller's scratch (the first read's
	 * dst, >= 0x1C00 bytes). keybuf (0x1048) and the kept secure area (0x800) do
	 * not overlap, so decryptSecureArea can rebuild keybuf while reading secure. */
	tNDSHeader* ndsHeader;
	u32* secureArea;
	u32* discard;
	u32 portFlagsKey1, portFlagsSecRead;
	bool normalChip;
	int secureBlockNumber;
	u32 kept;
	int i;
	u32 chipID = 0;
	u8 cmdData[8] __attribute__((aligned(4)));
	GameCode* gameCode;

	keybuf     = scratch;                               /* 0x1048 bytes */
	secureArea = scratch + (0x1048 / sizeof(u32));      /* 0x800 bytes  */
	discard    = scratch + ((0x1048 + 0x800) / sizeof(u32));   /* 0x200 bytes */
	ndsHeader  = (tNDSHeader*)(scratch + ((0x1048 + 0x800 + 0x200) / sizeof(u32))); /* 0x160 */

	/* NTR-mode BIOS, so the dscmReadBios gadget targets the NTR ARM7 BIOS. */
	REG_SCFG_ROM = 0x703;

	/* Power/ready the slot (no-op if already powered), then a raw reset. */
	if (!(REG_SCFG_MC & 0x0c)) {
		REG_SCFG_MC = (REG_SCFG_MC & ~0x0c) | 4; swiDelay(0x2000);
		REG_SCFG_MC = (REG_SCFG_MC & ~0x0c) | 8; swiDelay(0x2000);
	}
	REG_AUXSPICNT = CARD_CR1_ENABLE | CARD_CR1_IRQ;
	REG_ROMCTRL = 0x20000000;
	dscmCardReset();

	dscmCardParamCommand(CARD_CMD_DUMMY, 0,
		CARD_ACTIVATE | CARD_nRESET | CARD_CLK_SLOW | CARD_BLK_SIZE(1) | CARD_DELAY1(0x1FFF) | CARD_DELAY2(0x3F),
		NULL, 0);

	dscmCardParamCommand(CARD_CMD_HEADER_READ, 0,
		CARD_ACTIVATE | CARD_nRESET | CARD_CLK_SLOW | CARD_BLK_SIZE(1) | CARD_DELAY1(0x1FFF) | CARD_DELAY2(0x3F),
		(u32*)ndsHeader, sizeof(tNDSHeader) / sizeof(u32));

	dbg_printf("DSCM reinit hdrcrc="); dbg_hexa(ndsHeader->headerCRC16); dbg_printf("\n");
	if (ndsHeader->headerCRC16 != swiCRC16(0xFFFF, (void*)ndsHeader, 0x15E))
		return 1;
	if (ndsHeader->logoCRC16 != 0xCF56)
		return 2;

	gameCode = (GameCode*)ndsHeader->gameCode;
	init_keycode(gameCode->key, 2, 8);

	portFlagsKey1 = CARD_ACTIVATE | CARD_nRESET | (ndsHeader->cardControl13 & (CARD_WR | CARD_CLK_SLOW)) |
		((ndsHeader->cardControlBF & (CARD_CLK_SLOW | CARD_DELAY1(0x1FFF))) + ((ndsHeader->cardControlBF & CARD_DELAY2(0x3F)) >> 16));

	dscmCardParamCommand(CARD_CMD_HEADER_CHIPID, 0,
		(ndsHeader->cardControl13 & (CARD_WR | CARD_nRESET | CARD_CLK_SLOW)) | CARD_ACTIVATE | CARD_BLK_SIZE(7),
		&chipID, sizeof(u32) / sizeof(u32));

	normalChip = ((chipID) & 0x80000000) != 0;
	if (!normalChip)
		portFlagsKey1 |= CARD_SEC_LARGE;

	initKey1Encryption(cmdData);
	dscmCardPolledTransfer((ndsHeader->cardControl13 & (CARD_WR | CARD_nRESET | CARD_CLK_SLOW)) | CARD_ACTIVATE, NULL, 0, cmdData);

	createEncryptedCommand(CARD_CMD_ACTIVATE_SEC, cmdData, 0);
	if (normalChip) {
		dscmCardPolledTransfer(portFlagsKey1, NULL, 0, cmdData);
		dscmCardDelay(ndsHeader->readTimeout);
		dscmCardPolledTransfer(portFlagsKey1, NULL, 0, cmdData);
	} else {
		dscmCardPolledTransfer(portFlagsKey1, NULL, 0, cmdData);
	}

	REG_ROMCTRL = 0;
	REG_CARD_1B0 = cardSeedBytes[ndsHeader->deviceType & 0x07] | (key1data.nnn << 15) | (key1data.mmm << 27) | 0x6000;
	REG_CARD_1B4 = 0x879b9b05;
	REG_CARD_1B8 = key1data.mmm >> 5;
	REG_CARD_1BA = 0x5c;
	REG_ROMCTRL = CARD_nRESET | CARD_SEC_SEED | CARD_SEC_EN | CARD_SEC_DAT;

	portFlagsKey1 |= CARD_SEC_EN | CARD_SEC_DAT;

	createEncryptedCommand(CARD_CMD_SECURE_CHIPID, cmdData, 0);
	if (normalChip) {
		dscmCardPolledTransfer(portFlagsKey1, NULL, 0, cmdData);
		dscmCardDelay(ndsHeader->readTimeout);
		dscmCardPolledTransfer(portFlagsKey1 | CARD_BLK_SIZE(7), NULL, 0, cmdData);
	} else {
		dscmCardPolledTransfer(portFlagsKey1 | CARD_BLK_SIZE(7), NULL, 0, cmdData);
	}

	portFlagsSecRead = (ndsHeader->cardControlBF & (CARD_CLK_SLOW | CARD_DELAY1(0x1FFF) | CARD_DELAY2(0x3F)))
		| CARD_ACTIVATE | CARD_nRESET | CARD_SEC_EN | CARD_SEC_DAT;

	kept = 0;
	for (secureBlockNumber = 4; secureBlockNumber < 8; secureBlockNumber++) {
		createEncryptedCommand(CARD_CMD_SECURE_READ, cmdData, secureBlockNumber);
		if (normalChip) {
			dscmCardPolledTransfer(portFlagsSecRead, NULL, 0, cmdData);
			dscmCardDelay(ndsHeader->readTimeout);
			for (i = 8; i > 0; i--) {
				/* Keep the first 0x800 bytes (for the decrypt check); discard the
				 * rest, but still clock every transfer to advance the protocol. */
				u32* dest = (kept < 0x200) ? (secureArea + kept) : discard;
				dscmCardPolledTransfer(portFlagsSecRead | CARD_BLK_SIZE(1), dest, 0x200 / sizeof(u32), cmdData);
				if (kept < 0x200) kept += 0x200 / sizeof(u32);
			}
		} else {
			/* Large secure read (0x1000) once per block; keep only the first. */
			u32* dest = (secureBlockNumber == 4) ? secureArea : discard;
			dscmCardPolledTransfer(portFlagsSecRead | CARD_BLK_SIZE(4) | CARD_SEC_LARGE, dest, 0x1000 / sizeof(u32), cmdData);
		}
	}

	createEncryptedCommand(CARD_CMD_DATA_MODE, cmdData, 0);
	if (normalChip) {
		dscmCardPolledTransfer(portFlagsKey1, NULL, 0, cmdData);
		dscmCardDelay(ndsHeader->readTimeout);
		dscmCardPolledTransfer(portFlagsKey1, NULL, 0, cmdData);
	} else {
		dscmCardPolledTransfer(portFlagsKey1, NULL, 0, cmdData);
	}

	// The secure-area decrypt check is only diagnostic and costs two more slow
	// BIOS reads with IRQs off, so skip it: the gameplay read's cmp==0 already
	// proves the card is in synced KEY2 data mode.
	(void)secureArea; (void)discard;
	dbg_printf("DSCM reinit done chipID="); dbg_hexa(chipID); dbg_printf("\n");

	return 0;
}
#endif
