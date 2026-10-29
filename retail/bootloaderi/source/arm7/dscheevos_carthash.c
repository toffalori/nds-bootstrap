/*
 * Cheevotron cart mode: the inserted cartridge's RetroAchievements hash.
 *
 * RetroAchievements identifies a DS game by rcheevos' hash of its ROM dump:
 * MD5 over the header's first 0x160 bytes, the ARM9 binary, the ARM7 binary
 * and the 0xA00-byte icon/title block, each as laid out in the dump
 * (rcheevos src/hash/rc_hash_rom.c, rc_hash_nintendo_ds). A dump stores the
 * secure area as the cartridge returns it in KEY1 mode: the first 2 KB
 * (0x4000..0x47FF) still encrypted, "encryObj" included. The card boot
 * decrypts those 2 KB in place and blanks the ID before the game runs, so it
 * keeps a raw copy for this hash; everything else is the same bytes a dumper
 * reads.
 *
 * MD5: RFC 1321, written out plainly; the bootloader has no libc hash and no
 * room for a general one.
 */
#include <nds/ndstypes.h>
#include <string.h>
#include "dscheevos_cardboot.h"

typedef struct {
	u32 a, b, c, d;
	u32 lo, hi;        /* bytes hashed so far */
	u8 buf[64];        /* the block being filled */
} md5_t;

#define F(x, y, z) ((z) ^ ((x) & ((y) ^ (z))))
#define G(x, y, z) ((y) ^ ((z) & ((x) ^ (y))))
#define H(x, y, z) ((x) ^ (y) ^ (z))
#define I(x, y, z) ((y) ^ ((x) | ~(z)))
#define ROTL(v, s) (((v) << (s)) | ((v) >> (32 - (s))))

static const u32 md5_k[64] = {
	0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
	0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
	0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
	0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
	0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
	0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
	0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
	0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};
static const u8 md5_r[64] = {
	7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
	5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
	4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
	6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

/* One 64-byte block, read byte-wise: the data may sit at any alignment. */
static void md5_block(md5_t* m, const u8* p) {
	u32 x[16];
	u32 a = m->a, b = m->b, c = m->c, d = m->d;
	int i;
	for (i = 0; i < 16; i++) {
		x[i] = (u32)p[i * 4] | ((u32)p[i * 4 + 1] << 8) | ((u32)p[i * 4 + 2] << 16) | ((u32)p[i * 4 + 3] << 24);
	}
	for (i = 0; i < 64; i++) {
		u32 f, g, t;
		if (i < 16)      { f = F(b, c, d); g = i; }
		else if (i < 32) { f = G(b, c, d); g = (5 * i + 1) & 15; }
		else if (i < 48) { f = H(b, c, d); g = (3 * i + 5) & 15; }
		else             { f = I(b, c, d); g = (7 * i) & 15; }
		t = d;
		d = c;
		c = b;
		b = b + ROTL(a + f + md5_k[i] + x[g], md5_r[i]);
		a = t;
	}
	m->a += a;
	m->b += b;
	m->c += c;
	m->d += d;
}

static void md5_init(md5_t* m) {
	m->a = 0x67452301;
	m->b = 0xefcdab89;
	m->c = 0x98badcfe;
	m->d = 0x10325476;
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
	w[0] = m->a; w[1] = m->b; w[2] = m->c; w[3] = m->d;
	for (i = 0; i < 16; i++) {
		u8 byte = (u8)(w[i / 4] >> (8 * (i & 3)));
		out[i * 2] = hex[byte >> 4];
		out[i * 2 + 1] = hex[byte & 15];
	}
	out[32] = 0;
}

/* The bytes of ROM [start, end) as a dump holds them: the raw secure copy
 * for 0x4000..0x47FF, the RAM copy of the binary for the rest. */
static void hash_binary(md5_t* m, const u8* ram, u32 romOffset, u32 size, const u8* secureRaw) {
	const u32 end = romOffset + size;
	u32 pos = romOffset;
	while (pos < end) {
		u32 stop = end;
		if (pos < DSCM_CART_SECURE_RAW_START && stop > DSCM_CART_SECURE_RAW_START) {
			stop = DSCM_CART_SECURE_RAW_START;
		} else if (pos >= DSCM_CART_SECURE_RAW_START && pos < DSCM_CART_SECURE_RAW_END) {
			if (stop > DSCM_CART_SECURE_RAW_END) {
				stop = DSCM_CART_SECURE_RAW_END;
			}
		}
		if (pos >= DSCM_CART_SECURE_RAW_START && pos < DSCM_CART_SECURE_RAW_END) {
			md5_append(m, secureRaw + (pos - DSCM_CART_SECURE_RAW_START), stop - pos);
		} else {
			md5_append(m, ram + (pos - romOffset), stop - pos);
		}
		pos = stop;
	}
}

void dscmCartHashRom(const u8* header, const u8* arm9, const u8* arm7, const u8* secureRaw, const u8* icon, md5_t* m, char out[33]) {
	const u32 arm9Offset = *(const u32*)(header + 0x20);
	const u32 arm9Size   = *(const u32*)(header + 0x2C);
	const u32 arm7Offset = *(const u32*)(header + 0x30);
	const u32 arm7Size   = *(const u32*)(header + 0x3C);
	md5_init(m);
	md5_append(m, header, 0x160);
	hash_binary(m, arm9, arm9Offset, arm9Size, secureRaw);
	hash_binary(m, arm7, arm7Offset, arm7Size, secureRaw);
	md5_append(m, icon, DSCM_CART_ICON_SIZE);
	md5_finish(m, out);
}

#ifndef DSCM_CART_HASH_HOST
/* The cartridge now in the slot, after loadBinary_ARM7_cart read its
 * binaries into RAM and dscmCartInit kept the raw secure area. The icon
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
	                DSCM_CART_SECURE_RAW, icon, (md5_t*)DSCM_CART_MD5, out);
}
#endif
