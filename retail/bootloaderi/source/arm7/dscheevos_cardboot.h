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

#endif
