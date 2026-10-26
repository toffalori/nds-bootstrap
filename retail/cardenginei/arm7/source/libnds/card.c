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

static void dscmCardWriteCommand(const u8* command) {
	int index;
	REG_AUXSPICNTH = CARD_CR1_ENABLE | CARD_CR1_IRQ;
	for (index = 0; index < 8; index++) {
		REG_CARD_COMMAND[7 - index] = command[index];
	}
}

static void dscmCardPolledTransfer(u32 flags, u32* destination, u32 length, const u8* command) {
	u32 data;
	dscmCardWriteCommand(command);
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
	dscmCardParamCommand(CARD_CMD_DATA_READ, blockBase, cmdFlags, block, 0x200 / sizeof(u32));
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
}
