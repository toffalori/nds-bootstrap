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
#include <string.h>

static void cardWriteCommand(const u8* command) {
	int index;
	REG_AUXSPICNTH = CARD_CR1_ENABLE | CARD_CR1_IRQ;
	for (index = 0; index < 8; index++) {
		REG_CARD_COMMAND[7 - index] = command[index];
	}
}

static void cardPolledTransfer(u32 flags, u32* destination, u32 length, const u8* command) {
	u32 data;
	cardWriteCommand(command);
	REG_ROMCTRL = flags;
	u32* target = destination + length;
	do {
		if (REG_ROMCTRL & CARD_DATA_READY) {
			data = REG_CARD_DATA_RD;
			if (destination != NULL && destination < target)
				*destination++ = data;
		}
	} while (REG_ROMCTRL & CARD_BUSY);
}

static void cardParamCommand(u8 command, u32 parameter, u32 flags, u32* destination, u32 length) {
	u8 cmdData[8];
	cmdData[7] = (u8)command;
	cmdData[6] = (u8)(parameter >> 24);
	cmdData[5] = (u8)(parameter >> 16);
	cmdData[4] = (u8)(parameter >> 8);
	cmdData[3] = (u8)(parameter >> 0);
	cmdData[2] = 0;
	cmdData[1] = 0;
	cmdData[0] = 0;
	cardPolledTransfer(flags, destination, length, cmdData);
}

/* Read `len` bytes from ROM offset `src` of the live cartridge into `dst`.
 * portFlags is the header's cardControl13 (header +0x60). Reads whole aligned
 * 0x200 blocks into a scratch and copies out just the requested range, so an
 * unaligned src or a non-0x200 length never overruns dst. The card mirrors the
 * secure area (<0x8000) to 0x8000+(addr&0x1FF) by itself, so no special case. */
void dscmCartReadData(u32 src, u8* dst, u32 len, u32 portFlags) {
	static u32 block[0x200 / sizeof(u32)] __attribute__((aligned(4)));
	const u32 cmdFlags = (portFlags & ~CARD_BLK_SIZE(7)) | CARD_ACTIVATE | CARD_nRESET | CARD_BLK_SIZE(1);
	while (len > 0) {
		u32 blockBase = src & ~0x1FFu;
		u32 off = src & 0x1FFu;
		u32 n = 0x200u - off;
		if (n > len) n = len;
		cardParamCommand(CARD_CMD_DATA_READ, blockBase, cmdFlags, block, 0x200 / sizeof(u32));
		memcpy(dst, (u8*)block + off, n);
		dst += n;
		src += n;
		len -= n;
	}
}
