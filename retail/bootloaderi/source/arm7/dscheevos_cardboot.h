/*
 * DScheevos / Cheevotron cartridge boot: read and boot a real Slot-1 DS
 * cartridge, so the streamer can run on the real cart (not only an SD ROM).
 * The cart-read code is ported from NitroHax (Chishm, GPL-3): KEY1 handshake,
 * secure-area decrypt, header, chip ID, and reading the game's data.
 */
#ifndef DSCHEEVOS_CARDBOOT_H
#define DSCHEEVOS_CARDBOOT_H

#include <nds/ndstypes.h>
#include "nds_header.h"

enum {
	DSCM_CART_OK = 0,
	DSCM_CART_ERR_HEAD_CRC,
	DSCM_CART_ERR_LOGO_CRC,
};

/* KEY1-init the inserted cartridge, read its header into ndsHeader and its chip
 * ID into chipID. Returns DSCM_CART_OK or a DSCM_CART_ERR_*. */
int dscmCartInit(tNDSHeader* ndsHeader, u32* chipID);

/* Read size bytes of the cartridge at offset src into dest (word aligned).
 * The secure area (0x4000..0x7FFF) comes from the decrypted copy cardInit kept. */
void dscmCartRead(u32 src, u32* dest, size_t size);

/* The cartridge's RetroAchievements hash (dscheevos_carthash.c), computed as
 * rcheevos hashes a dump of it. Buffers in the DScheevos carve-out, like
 * cardInit's: the raw (still encrypted) first 2 KB of the secure area, kept
 * before the in-place decrypt; the icon block read for the hash; the MD5
 * state. The hash itself sits past the session's room, where nothing writes
 * before dscheevosSetup copies it into the session (main.arm7.c clears
 * 0x02F00000 up before then). */
#define DSCM_CART_SECURE_RAW_START 0x4000
#define DSCM_CART_SECURE_RAW_END   0x4800
#define DSCM_CART_DATA_START       0x4000  /* dscmCartRead serves nothing below */
#define DSCM_CART_ICON_SIZE        0xA00
#define DSCM_CART_SECURE_RAW ((u8*)0x02F08000)
#define DSCM_CART_SCRATCH    ((u8*)0x02F09000)
#define DSCM_CART_MD5        ((void*)0x02F0A000)
#define DSCM_CART_HASH       ((char*)0x02E60400)  /* 33 bytes, lower-case hex, NUL */
void dscmCartHash(const tNDSHeader* ndsHeader, char out[33]);

#endif
