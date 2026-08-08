// NerdSha256.h — optimized software SHA-256d for mining, ported from
// BitMaker-hub/NerdMiner_v2 src/ShaTests/nerdSHA256plus.h (MIT license,
// (c) 2023 Bitmaker; based on Blockstream Jade's shaLib). See NerdSha256.cpp.
#pragma once
#include "config.h"
#if WITH_MINER

#include <Arduino.h>
#include <stdint.h>

// Midstate: hash the header's first 64-byte block into digest[8].
IRAM_ATTR void nerd_mids(uint32_t* digest, const uint8_t* dataIn);

// Precompute the nonce-independent parts of the second block (dataIn = header
// bytes 64..79 zone) into bake[16] (15 words used).
IRAM_ATTR void nerd_sha256_bake(const uint32_t* digest, const uint8_t* dataIn,
                                uint32_t* bake);

// Double-hash one nonce (already written into dataIn at offset 12). Returns
// false on the 16-bit early exit (hash cannot meet any share target); returns
// true with the full digest in doubleHash[32] otherwise.
IRAM_ATTR bool nerd_sha256d_baked(const uint32_t* digest, const uint8_t* dataIn,
                                  const uint32_t* bake, uint8_t* doubleHash);

#endif  // WITH_MINER
