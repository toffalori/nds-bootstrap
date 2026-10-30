/*
 * Cheevotron cart mode: the MD5 block function for the cartridge's
 * RetroAchievements hash (dscheevos_carthash.c), on its own as ARM code (the
 * .arm.c name: devkitARM's rules build it with -marm). The bootloader is
 * Thumb otherwise, and Thumb-1 needs about twice the instructions for MD5's
 * 32-bit adds and rotates; the rounds are written out, and the 64 bytes are
 * copied to the stack once instead of read byte by byte from main RAM four
 * times over. In Thumb with byte reads, HeartGold's 0.9 MB took 1.56 s.
 * RFC 1321, as before; the result is the same.
 */
#include <nds/ndstypes.h>

#ifndef LOADERTYPE2   /* bootloader2 boots no cartridge (dscheevos_carthash.c) */

#define F(x, y, z) ((z) ^ ((x) & ((y) ^ (z))))
#define G(x, y, z) ((y) ^ ((z) & ((x) ^ (y))))
#define H(x, y, z) ((x) ^ (y) ^ (z))
#define I(x, y, z) ((y) ^ ((x) | ~(z)))
#define ROTL(v, s) (((v) << (s)) | ((v) >> (32 - (s))))
#define STEP(f, a, b, c, d, xi, k, s) \
	(a) += f((b), (c), (d)) + (xi) + (u32)(k); \
	(a) = ROTL((a), (s)) + (b);

/* state: a, b, c, d; p: 64 bytes at any alignment. */
void dscmMd5Block(u32* state, const u8* p) {
	u32 x[16];
	u32 a = state[0], b = state[1], c = state[2], d = state[3];
	if (((u32)p & 3) == 0) {
		const u32* w = (const u32*)p;
		for (int i = 0; i < 16; i++) {
			x[i] = w[i];
		}
	} else {
		for (int i = 0; i < 16; i++) {
			x[i] = (u32)p[i * 4] | ((u32)p[i * 4 + 1] << 8) | ((u32)p[i * 4 + 2] << 16) | ((u32)p[i * 4 + 3] << 24);
		}
	}

	STEP(F, a, b, c, d, x[0],  0xd76aa478, 7)
	STEP(F, d, a, b, c, x[1],  0xe8c7b756, 12)
	STEP(F, c, d, a, b, x[2],  0x242070db, 17)
	STEP(F, b, c, d, a, x[3],  0xc1bdceee, 22)
	STEP(F, a, b, c, d, x[4],  0xf57c0faf, 7)
	STEP(F, d, a, b, c, x[5],  0x4787c62a, 12)
	STEP(F, c, d, a, b, x[6],  0xa8304613, 17)
	STEP(F, b, c, d, a, x[7],  0xfd469501, 22)
	STEP(F, a, b, c, d, x[8],  0x698098d8, 7)
	STEP(F, d, a, b, c, x[9],  0x8b44f7af, 12)
	STEP(F, c, d, a, b, x[10], 0xffff5bb1, 17)
	STEP(F, b, c, d, a, x[11], 0x895cd7be, 22)
	STEP(F, a, b, c, d, x[12], 0x6b901122, 7)
	STEP(F, d, a, b, c, x[13], 0xfd987193, 12)
	STEP(F, c, d, a, b, x[14], 0xa679438e, 17)
	STEP(F, b, c, d, a, x[15], 0x49b40821, 22)

	STEP(G, a, b, c, d, x[1],  0xf61e2562, 5)
	STEP(G, d, a, b, c, x[6],  0xc040b340, 9)
	STEP(G, c, d, a, b, x[11], 0x265e5a51, 14)
	STEP(G, b, c, d, a, x[0],  0xe9b6c7aa, 20)
	STEP(G, a, b, c, d, x[5],  0xd62f105d, 5)
	STEP(G, d, a, b, c, x[10], 0x02441453, 9)
	STEP(G, c, d, a, b, x[15], 0xd8a1e681, 14)
	STEP(G, b, c, d, a, x[4],  0xe7d3fbc8, 20)
	STEP(G, a, b, c, d, x[9],  0x21e1cde6, 5)
	STEP(G, d, a, b, c, x[14], 0xc33707d6, 9)
	STEP(G, c, d, a, b, x[3],  0xf4d50d87, 14)
	STEP(G, b, c, d, a, x[8],  0x455a14ed, 20)
	STEP(G, a, b, c, d, x[13], 0xa9e3e905, 5)
	STEP(G, d, a, b, c, x[2],  0xfcefa3f8, 9)
	STEP(G, c, d, a, b, x[7],  0x676f02d9, 14)
	STEP(G, b, c, d, a, x[12], 0x8d2a4c8a, 20)

	STEP(H, a, b, c, d, x[5],  0xfffa3942, 4)
	STEP(H, d, a, b, c, x[8],  0x8771f681, 11)
	STEP(H, c, d, a, b, x[11], 0x6d9d6122, 16)
	STEP(H, b, c, d, a, x[14], 0xfde5380c, 23)
	STEP(H, a, b, c, d, x[1],  0xa4beea44, 4)
	STEP(H, d, a, b, c, x[4],  0x4bdecfa9, 11)
	STEP(H, c, d, a, b, x[7],  0xf6bb4b60, 16)
	STEP(H, b, c, d, a, x[10], 0xbebfbc70, 23)
	STEP(H, a, b, c, d, x[13], 0x289b7ec6, 4)
	STEP(H, d, a, b, c, x[0],  0xeaa127fa, 11)
	STEP(H, c, d, a, b, x[3],  0xd4ef3085, 16)
	STEP(H, b, c, d, a, x[6],  0x04881d05, 23)
	STEP(H, a, b, c, d, x[9],  0xd9d4d039, 4)
	STEP(H, d, a, b, c, x[12], 0xe6db99e5, 11)
	STEP(H, c, d, a, b, x[15], 0x1fa27cf8, 16)
	STEP(H, b, c, d, a, x[2],  0xc4ac5665, 23)

	STEP(I, a, b, c, d, x[0],  0xf4292244, 6)
	STEP(I, d, a, b, c, x[7],  0x432aff97, 10)
	STEP(I, c, d, a, b, x[14], 0xab9423a7, 15)
	STEP(I, b, c, d, a, x[5],  0xfc93a039, 21)
	STEP(I, a, b, c, d, x[12], 0x655b59c3, 6)
	STEP(I, d, a, b, c, x[3],  0x8f0ccc92, 10)
	STEP(I, c, d, a, b, x[10], 0xffeff47d, 15)
	STEP(I, b, c, d, a, x[1],  0x85845dd1, 21)
	STEP(I, a, b, c, d, x[8],  0x6fa87e4f, 6)
	STEP(I, d, a, b, c, x[15], 0xfe2ce6e0, 10)
	STEP(I, c, d, a, b, x[6],  0xa3014314, 15)
	STEP(I, b, c, d, a, x[13], 0x4e0811a1, 21)
	STEP(I, a, b, c, d, x[4],  0xf7537e82, 6)
	STEP(I, d, a, b, c, x[11], 0xbd3af235, 10)
	STEP(I, c, d, a, b, x[2],  0x2ad7d2bb, 15)
	STEP(I, b, c, d, a, x[9],  0xeb86d391, 21)

	state[0] += a;
	state[1] += b;
	state[2] += c;
	state[3] += d;
}
#endif
