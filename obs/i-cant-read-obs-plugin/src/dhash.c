#include "dhash.h"

uint64_t dhash_compute_9x8(const uint8_t gray9x8[72])
{
	uint64_t hash = 0;
	/*
     * For each row: compare 8 adjacent pixel pairs horizontally.
     * Bit is set when left pixel is strictly greater than right.
     * Bit ordering: row 0 → bits 0-7, row 1 → bits 8-15, ...
     */
	for (int y = 0; y < 8; y++)
		for (int x = 0; x < 8; x++)
			if (gray9x8[y * 9 + x] > gray9x8[y * 9 + x + 1])
				hash |= (uint64_t)1 << (y * 8 + x);
	return hash;
}

int dhash_hamming64(uint64_t a, uint64_t b)
{
	return __builtin_popcountll(a ^ b);
}
