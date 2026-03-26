#pragma once
#include <stdint.h>

/*
 * Compute a 64-bit difference hash from a 9×8 grayscale array.
 * Layout: row-major, gray9x8[y*9 + x].
 * Matches imagehash.dhash(img, hash_size=8) on a pre-downsampled image.
 */
uint64_t dhash_compute_9x8(const uint8_t gray9x8[72]);
int dhash_hamming64(uint64_t a, uint64_t b);
