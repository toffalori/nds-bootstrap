/*
    Cartridge boot for DScheevos / Cheevotron: read a real Slot-1 DS cartridge.
    Ported from NitroHax (read_card.c, encryption.c) by Michael "Chishm"
    Chisholm.

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

#include "dscheevos_cardboot.h"
#include "common.h"   // arm9_stateFlag, ARM9_DSCM_CARD*, DSCM_CARD_OP_*

#include <nds/ndstypes.h>
#include <nds/card.h>
#include <nds/dma.h>
#include <nds/timers.h>
#include <nds/bios.h>
#include <string.h>

#define CARD_SECURE_AREA_OFFSET (0x4000)
#define CARD_SECURE_AREA_SIZE   (0x4000)
#define CARD_DATA_OFFSET        (0x8000)
#define CARD_DATA_BLOCK_SIZE    (0x200)

/* nds-bootstrap's libnds card helpers (source/arm7/libnds/card.c) */
extern void cardParamCommand(u8 command, u32 parameter, u32 flags, u32* destination, u32 length);
extern void cardPolledTransfer(u32 flags, u32* destination, u32 length, const u8* command);
/* the ARM7 BIOS reader (dscheevos_read_bios.s) */
extern void dscmReadBios(u8* dest, u32 src, u32 size);
/* bootloader debug log (debug_file.h) */
extern u32 dbg_printf(const char* message);
extern u32 dbg_hexa(u32 n);
/* dscheevos_hooks.c */
extern bool dscheevosCartNative(void);

/* ---- KEY1 blowfish (NitroHax encryption.c) ---- */

#define KEYSIZE 0x1048
static u32 keycode[3];
/* The blowfish table (4 KB) and the decrypted secure area (16 KB) are too big
 * for the bootloader's VRAM .bss. Put them in the DScheevos module carve-out
 * (0x02D80000..0x02F80000), which is reserved and free during the cart read:
 * the module only loads there later, at dscheevosSetup, after the cart boot is
 * done with these buffers. */
static u32* const keybuf = (u32*)0x02F04100;

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

static void crypt_64bit_down(u32* ptr) {
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

/* ---- cart init and read (NitroHax read_card.c) ---- */

typedef union {
	char title[4];
	u32 key;
} GameCode;

static u32 portFlags = 0;
static u32* const secureAreaData = (u32*)0x02F00000;  /* see keybuf: in the module carve-out */

static const u8 cardSeedBytes[] = { 0xE8, 0x4D, 0x5A, 0xB1, 0x17, 0x8F, 0x99, 0xD5 };

static u32 getRandomNumber(void) { return 4; }  /* NitroHax: fixed, fine for KEY1 setup */

static void decryptSecureArea(u32 gameCode, u32* secureArea) {
	int i;
	init_keycode(gameCode, 2, 8);
	crypt_64bit_down(secureArea);
	init_keycode(gameCode, 3, 8);
	for (i = 0; i < 0x200; i += 2)
		crypt_64bit_down(secureArea + i);
}

static struct {
	unsigned int iii, jjj, kkkkk, llll, mmm, nnn;
} key1data;

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

// Timer 2: timers 0 and 1 are the bootloader's boot clock (arm7_main), which
// this delay stopped on timer 0, so the log's stamps lost time from here on.
static void cardDelayArm7(u16 readTimeout) {
	TIMER_DATA(2) = 0 - (((readTimeout & 0x3FFF) + 3));
	TIMER_CR(2) = TIMER_DIV_256 | TIMER_ENABLE;
	while (TIMER_DATA(2) != 0xFFFF);
	TIMER_CR(2) = 0;
	TIMER_DATA(2) = 0;
}

/* Native cart mode: the DSi keeps a KEY2 session per CPU, and the game reads
 * its card from the ARM9. So from the KEY2 seed on, the handshake's transfers
 * and every card read after it run on the ARM9 (main.arm9.c): the ARM7 lays
 * them out as a script (DSCM_CARD_OP_*, common.h) in the module carve-out,
 * free during the cart boot like keybuf. Engine mode keeps it all on the ARM7,
 * whose card engine then serves the game's reads. */
static bool toArm9 = false;
static u32* const cardScript = (u32*)0x02F0C000;
static u32 scriptLen = 0;
#define DSCM_CART_BOUNCE      ((u32*)0x02F10000)   /* ARM9 reads for a WRAM destination */
#define DSCM_CART_BOUNCE_SIZE 0x10000

#ifdef LOADERTYPE2
/* bootloaderi2's ARM9 side has no card helpers: there the whole handshake
 * stays on the ARM7 (native cart mode runs in bootloaderi only). These only
 * satisfy the references below, which never run. */
volatile u32 arm9_cardArgs[4];
volatile u32 arm9_cardProbe;
volatile u32 arm9_cardProbeFlags;
#define dscheevosCartNative() false
#endif

static void arm9Do(int state) {
	while (arm9_stateFlag != ARM9_READY);
	arm9_stateFlag = state;
	while (arm9_stateFlag != ARM9_READY);
}

/* A transfer: ARM7 now, or appended to the ARM9's script. words = the reply
 * words kept at dest. */
static void cardXfer(const u8* cmdData, u32 flags, u32* dest, u32 words) {
	if (!toArm9) {
		cardPolledTransfer(flags, dest, words, cmdData);
		return;
	}
	u32* s = cardScript + scriptLen;
	s[0] = DSCM_CARD_OP_XFER;
	s[1] = cmdData[7] | cmdData[6] << 8 | cmdData[5] << 16 | (u32)cmdData[4] << 24;   // as at 0x040001A8
	s[2] = cmdData[3] | cmdData[2] << 8 | cmdData[1] << 16 | (u32)cmdData[0] << 24;
	s[3] = flags;
	s[4] = (u32)dest;
	s[5] = dest ? words : 0;
	scriptLen += 6;
}

/* The card's settle delay: in VCOUNT changes on the ARM9 (a line is 63.6 us,
 * a timer tick at /256 7.6 us: ticks / 8 is a little longer, +2 for the line
 * the wait starts in). */
static void cardDelay(u16 readTimeout) {
	if (!toArm9) {
		cardDelayArm7(readTimeout);
		return;
	}
	cardScript[scriptLen] = DSCM_CARD_OP_WAIT;
	cardScript[scriptLen + 1] = ((((u32)readTimeout & 0x3FFF) + 3) >> 3) + 2;
	scriptLen += 2;
}

int dscmCartInit(tNDSHeader* ndsHeader, u32* chipID) {
	u32 portFlagsKey1, portFlagsSecRead;
	bool normalChip;
	u32* secureArea;
	int secureBlockNumber;
	int i;
	u8 cmdData[8] __attribute__((aligned));
	GameCode* gameCode;

	cardParamCommand(CARD_CMD_DUMMY, 0,
		CARD_ACTIVATE | CARD_nRESET | CARD_CLK_SLOW | CARD_BLK_SIZE(1) | CARD_DELAY1(0x1FFF) | CARD_DELAY2(0x3F),
		NULL, 0);

	cardParamCommand(CARD_CMD_HEADER_READ, 0,
		CARD_ACTIVATE | CARD_nRESET | CARD_CLK_SLOW | CARD_BLK_SIZE(1) | CARD_DELAY1(0x1FFF) | CARD_DELAY2(0x3F),
		(u32*)ndsHeader, sizeof(tNDSHeader));

	dbg_printf("  cart hdr read, crc=");
	dbg_hexa(ndsHeader->headerCRC16);
	dbg_printf("\n");
	if (ndsHeader->headerCRC16 != swiCRC16(0xFFFF, (void*)ndsHeader, 0x15E))
		return DSCM_CART_ERR_HEAD_CRC;
	if (ndsHeader->logoCRC16 != 0xCF56)
		return DSCM_CART_ERR_LOGO_CRC;

	gameCode = (GameCode*)ndsHeader->gameCode;
	dbg_printf("  cart readBios...\n");
	init_keycode(gameCode->key, 2, 8);
	dbg_printf("  cart keycode ok\n");

	portFlags = ndsHeader->cardControl13 & ~CARD_BLK_SIZE(7);
	portFlagsKey1 = CARD_ACTIVATE | CARD_nRESET | (ndsHeader->cardControl13 & (CARD_WR | CARD_CLK_SLOW)) |
		((ndsHeader->cardControlBF & (CARD_CLK_SLOW | CARD_DELAY1(0x1FFF))) + ((ndsHeader->cardControlBF & CARD_DELAY2(0x3F)) >> 16));

	cardParamCommand(CARD_CMD_HEADER_CHIPID, 0,
		(ndsHeader->cardControl13 & (CARD_WR | CARD_nRESET | CARD_CLK_SLOW)) | CARD_ACTIVATE | CARD_BLK_SIZE(7),
		chipID, sizeof(u32));

	normalChip = ((*chipID) & 0x80000000) != 0;
	if (!normalChip)
		portFlagsKey1 |= CARD_SEC_LARGE;

	initKey1Encryption(cmdData);
	cardPolledTransfer((ndsHeader->cardControl13 & (CARD_WR | CARD_nRESET | CARD_CLK_SLOW)) | CARD_ACTIVATE, NULL, 0, cmdData);

	createEncryptedCommand(CARD_CMD_ACTIVATE_SEC, cmdData, 0);
	if (normalChip) {
		cardPolledTransfer(portFlagsKey1, NULL, 0, cmdData);
		cardDelay(ndsHeader->readTimeout);
		cardPolledTransfer(portFlagsKey1, NULL, 0, cmdData);
	} else {
		cardPolledTransfer(portFlagsKey1, NULL, 0, cmdData);
	}

	// From the seed on: the ARM9 in native mode (see cardXfer)
	toArm9 = dscheevosCartNative();
	scriptLen = 0;
	{
		const u32 seed1B0 = cardSeedBytes[ndsHeader->deviceType & 0x07] | (key1data.nnn << 15) | (key1data.mmm << 27) | 0x6000;
		if (toArm9) {
			cardScript[0] = DSCM_CARD_OP_SEED;
			cardScript[1] = seed1B0;
			cardScript[2] = 0x879b9b05;
			cardScript[3] = key1data.mmm >> 5;
			cardScript[4] = 0x5c;
			scriptLen = 5;
		} else {
			REG_ROMCTRL = 0;
			REG_CARD_1B0 = seed1B0;
			REG_CARD_1B4 = 0x879b9b05;
			REG_CARD_1B8 = key1data.mmm >> 5;
			REG_CARD_1BA = 0x5c;
			REG_ROMCTRL = CARD_nRESET | CARD_SEC_SEED | CARD_SEC_EN | CARD_SEC_DAT;
		}
	}

	portFlagsKey1 |= CARD_SEC_EN | CARD_SEC_DAT;

	createEncryptedCommand(CARD_CMD_SECURE_CHIPID, cmdData, 0);
	if (normalChip) {
		cardXfer(cmdData, portFlagsKey1, NULL, 0);
		cardDelay(ndsHeader->readTimeout);
		cardXfer(cmdData, portFlagsKey1 | CARD_BLK_SIZE(7), NULL, 0);
	} else {
		cardXfer(cmdData, portFlagsKey1 | CARD_BLK_SIZE(7), NULL, 0);
	}

	secureArea = secureAreaData;
	portFlagsSecRead = (ndsHeader->cardControlBF & (CARD_CLK_SLOW | CARD_DELAY1(0x1FFF) | CARD_DELAY2(0x3F)))
		| CARD_ACTIVATE | CARD_nRESET | CARD_SEC_EN | CARD_SEC_DAT;

	for (secureBlockNumber = 4; secureBlockNumber < 8; secureBlockNumber++) {
		createEncryptedCommand(CARD_CMD_SECURE_READ, cmdData, secureBlockNumber);
		if (normalChip) {
			cardXfer(cmdData, portFlagsSecRead, NULL, 0);
			cardDelay(ndsHeader->readTimeout);
			for (i = 8; i > 0; i--) {
				cardXfer(cmdData, portFlagsSecRead | CARD_BLK_SIZE(1), secureArea, 0x200 / sizeof(u32));
				secureArea += 0x200 / sizeof(u32);
			}
		} else {
			cardXfer(cmdData, portFlagsSecRead | CARD_BLK_SIZE(4) | CARD_SEC_LARGE, secureArea, 0x1000 / sizeof(u32));
			secureArea += 0x1000 / sizeof(u32);
		}
	}

	createEncryptedCommand(CARD_CMD_DATA_MODE, cmdData, 0);
	if (normalChip) {
		cardXfer(cmdData, portFlagsKey1, NULL, 0);
		cardDelay(ndsHeader->readTimeout);
		cardXfer(cmdData, portFlagsKey1, NULL, 0);
	} else {
		cardXfer(cmdData, portFlagsKey1, NULL, 0);
	}

	if (toArm9) {
		cardScript[scriptLen] = DSCM_CARD_OP_END;
		arm9_cardArgs[0] = (u32)cardScript;
		arm9Do(ARM9_DSCM_CARDSCRIPT);
		dbg_printf("  cart KEY2 handshake on the ARM9, script words ");
		dbg_hexa(scriptLen);
		dbg_printf("\n");
	}

	dbg_printf("  cart secure read ok, enc=");
	dbg_hexa(secureAreaData[0]);
	dbg_hexa(secureAreaData[1]);
	decryptSecureArea(gameCode->key, secureAreaData);

	secureArea = secureAreaData;
	dbg_printf(" dec=");
	dbg_hexa(secureArea[0]);
	dbg_hexa(secureArea[1]);
	if (secureArea[0] == 0x72636e65 /*'encr'*/ && secureArea[1] == 0x6a624f79 /*'yObj'*/) {
		dbg_printf(" encryObj\n");
		secureArea[0] = 0xe7ffdeff;
		secureArea[1] = 0xe7ffdeff;
	} else {
		dbg_printf(" WIPED\n");
		for (i = 0; i < 0x200; i++)
			*secureArea++ = 0xe7ffdeff;
	}
	return DSCM_CART_OK;
}

void dscmCartArm9Unreset(void) {
#ifndef LOADERTYPE2
	if (dscheevosCartNative()) {
		arm9Do(ARM9_DSCM_CARDUNRESET);
	}
#endif
}

u32 dscmCartProbeFlags(void) {
	return (portFlags & ~CARD_BLK_SIZE(7)) | CARD_ACTIVATE | CARD_nRESET | CARD_BLK_SIZE(7);
}

u32 dscmCartProbeChipId(void) {
	u32 id = 0xFFFFFFFF;
	if (toArm9) {   // native mode: only the ARM9 may talk to the card
		arm9_cardProbeFlags = dscmCartProbeFlags();
		arm9_cardProbe = 0x11111111;
		arm9Do(ARM9_DSCM_CARDPROBE);
		return arm9_cardProbe;
	}
	cardParamCommand(0xB8, 0, dscmCartProbeFlags(), &id, 1);
	return id;
}

void dscmCartRead(u32 src, u32* dest, size_t size) {
	size_t readSize;
	if (src < CARD_SECURE_AREA_OFFSET) {
		return;
	} else if (src < CARD_DATA_OFFSET) {
		readSize = src + size < CARD_DATA_OFFSET ? size : CARD_DATA_OFFSET - src;
		memcpy(dest, (u8*)secureAreaData + src - CARD_SECURE_AREA_OFFSET, readSize);
		src += readSize;
		dest += readSize / sizeof(*dest);
		size -= readSize;
	}
	if (toArm9) {   // native mode: the ARM9 holds the KEY2 session
		// Main RAM is the same for both CPUs; WRAM (the file table goes to
		// 0x03700000) may not be, so that comes through a main RAM buffer.
		const bool mainRam = (u32)dest - 0x02000000 < 0x01000000;
		while (size > 0) {
			const u32 n = (mainRam || size < DSCM_CART_BOUNCE_SIZE) ? size : DSCM_CART_BOUNCE_SIZE;
			arm9_cardArgs[0] = src;
			arm9_cardArgs[1] = mainRam ? (u32)dest : (u32)DSCM_CART_BOUNCE;
			arm9_cardArgs[2] = n;
			arm9_cardArgs[3] = (portFlags & ~CARD_BLK_SIZE(7)) | CARD_ACTIVATE | CARD_nRESET | CARD_BLK_SIZE(1);
			arm9Do(ARM9_DSCM_CARDREAD);
			if (!mainRam) {
				memcpy(dest, DSCM_CART_BOUNCE, n);
			}
			src += n;
			dest += n / sizeof(*dest);
			size -= n;
		}
		return;
	}
	while (size > 0) {
		readSize = size < CARD_DATA_BLOCK_SIZE ? size : CARD_DATA_BLOCK_SIZE;
		// The card always sends a whole 0x200 block; cardPolledTransfer stores
		// only `length` words of it (it counts words, not bytes), so the last,
		// partial block of a read doesn't spill past the end of dest.
		cardParamCommand(CARD_CMD_DATA_READ, src,
			(portFlags & ~CARD_BLK_SIZE(7)) | CARD_ACTIVATE | CARD_nRESET | CARD_BLK_SIZE(1),
			dest, (readSize + 3) / 4);
		src += readSize;
		dest += readSize / sizeof(*dest);
		size -= readSize;
	}
}
