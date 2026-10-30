/*
 * Cheevotron cart mode: the inserted cartridge's RetroAchievements hash.
 *
 * RetroAchievements identifies a DS game by rcheevos' hash of its ROM dump:
 * MD5 over the header's first 0x160 bytes, the ARM9 binary, the ARM7 binary
 * and the 0xA00-byte icon/title block, each as laid out in the dump
 * (rcheevos src/hash/rc_hash_rom.c, rc_hash_nintendo_ds). The dumps
 * RetroAchievements lists (No-Intro) hold the secure area decrypted: the
 * first 2 KB (0x4000..0x47FF) decrypted and the "encryObj" ID replaced by
 * 0xE7FFDEFF twice, exactly as the card boot leaves it in RAM. Checked on
 * Super Mario 64 DS (Europe) and HeartGold (Germany): a hash over the still
 * encrypted 2 KB, as the card returns them in KEY1 mode, matches neither.
 * So the binaries are hashed as they are in RAM right after the card read.
 *
 * MD5: RFC 1321, written out plainly; the bootloader has no libc hash and no
 * room for a general one. The block function is ARM code in
 * dscheevos_md5.arm.c (speed).
 */
#include <nds/ndstypes.h>
#include <string.h>
#include "dscheevos_cardboot.h"

#ifdef LOADERTYPE2
/* bootloader2 runs only listed DSiWare titles, never a cartridge, and its
 * VRAM has no room for the ARM-code MD5. */
void dscmCartHash(const tNDSHeader* ndsHeader, char out[33]) {
	(void)ndsHeader;
	out[0] = 0;
}
#else
typedef struct {
	u32 s[4];          /* a, b, c, d */
	u32 lo, hi;        /* bytes hashed so far */
	u8 buf[64];        /* the block being filled */
} md5_t;

/* One 64-byte block at any alignment (dscheevos_md5.arm.c, ARM code). */
void dscmMd5Block(u32* state, const u8* p);
#define md5_block(m, p) dscmMd5Block((m)->s, (p))

static void md5_init(md5_t* m) {
	m->s[0] = 0x67452301;
	m->s[1] = 0xefcdab89;
	m->s[2] = 0x98badcfe;
	m->s[3] = 0x10325476;
	m->lo = 0;
	m->hi = 0;
}

static void md5_append(md5_t* m, const u8* p, u32 n) {
	u32 have = m->lo & 63;
	u32 lo = m->lo + n;
	if (lo < m->lo) {
		m->hi++;
	}
	m->lo = lo;
	if (have) {
		u32 take = 64 - have;
		if (take > n) {
			take = n;
		}
		memcpy(m->buf + have, p, take);
		have += take;
		p += take;
		n -= take;
		if (have < 64) {
			return;
		}
		md5_block(m, m->buf);
	}
	while (n >= 64) {
		md5_block(m, p);
		p += 64;
		n -= 64;
	}
	if (n) {
		memcpy(m->buf, p, n);
	}
}

static void md5_finish(md5_t* m, char out[33]) {
	static const char hex[] = "0123456789abcdef";
	u8 len[8];
	u8 pad = 0x80;
	u32 bits_lo = m->lo << 3, bits_hi = (m->hi << 3) | (m->lo >> 29);
	u32 w[4];
	int i;
	for (i = 0; i < 4; i++) {
		len[i] = (u8)(bits_lo >> (8 * i));
		len[4 + i] = (u8)(bits_hi >> (8 * i));
	}
	md5_append(m, &pad, 1);
	pad = 0;
	while ((m->lo & 63) != 56) {
		md5_append(m, &pad, 1);
	}
	md5_append(m, len, 8);
	w[0] = m->s[0]; w[1] = m->s[1]; w[2] = m->s[2]; w[3] = m->s[3];
	for (i = 0; i < 16; i++) {
		u8 byte = (u8)(w[i / 4] >> (8 * (i & 3)));
		out[i * 2] = hex[byte >> 4];
		out[i * 2 + 1] = hex[byte & 15];
	}
	out[32] = 0;
}

void dscmCartHashRom(const u8* header, const u8* arm9, const u8* arm7, const u8* icon, md5_t* m, char out[33]) {
	const u32 arm9Size = *(const u32*)(header + 0x2C);
	const u32 arm7Size = *(const u32*)(header + 0x3C);
	md5_init(m);
	md5_append(m, header, 0x160);
	md5_append(m, arm9, arm9Size);
	md5_append(m, arm7, arm7Size);
	md5_append(m, icon, DSCM_CART_ICON_SIZE);
	md5_finish(m, out);
}

#ifndef DSCM_CART_HASH_HOST
/* The cartridge now in the slot, after loadBinary_ARM7_cart read its
 * binaries into RAM (nothing has patched them yet). The icon
 * block is read from the card here; a ROM without one (icon offset 0) hashes
 * zeros for it, as rcheevos does when the read falls short. */
void dscmCartHash(const tNDSHeader* ndsHeader, char out[33]) {
	const u8* hb = (const u8*)ndsHeader;
	const u32 iconOffset = *(const u32*)(hb + 0x68);
	u8* icon = DSCM_CART_SCRATCH;
	memset(icon, 0, DSCM_CART_ICON_SIZE);
	if (iconOffset >= DSCM_CART_DATA_START) {
		dscmCartRead(iconOffset, (u32*)icon, DSCM_CART_ICON_SIZE);
	}
	dscmCartHashRom(hb, (const u8*)ndsHeader->arm9destination, (const u8*)ndsHeader->arm7destination,
	                icon, (md5_t*)DSCM_CART_MD5, out);
}
#endif
#endif
