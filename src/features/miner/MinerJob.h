// MinerJob.h — the pure math of turning a stratum job into hashable work:
// hex decoding, the coinbase/merkle root, the 80-byte block header, the network
// target, and share difficulty.
//
// Deliberately free of FreeRTOS, WiFi, mbedTLS and Arduino String so it can be
// compiled and checked on a host machine against real block data
// (tools/miner_selftest). MinerCore.cpp is the only firmware user.
#pragma once
#include "config.h"
#if WITH_MINER

#include <stdint.h>
#include <stddef.h>

// Longest coinbase (in bytes) a job may carry; its hex form is twice this.
#define MINER_MAX_COINBASE 1024

// Everything the hash workers need for one job.
struct MinerWork {
  uint8_t  header[128];   // 80-byte block header + SHA-256 padding
  uint32_t midstate[8];   // first 64-byte block, hashed once per job
  uint8_t  targetLE[32];  // network target, same byte order as the hash
};

// Decode `inLen` hex chars into bytes; returns the number of bytes written.
int minerHexToBytes(const char* in, size_t inLen, uint8_t* out);

// SHA-256 and the double-SHA bitcoin uses everywhere.
void minerSha256(const uint8_t* in, size_t len, uint8_t out[32]);
void minerSha256d(const uint8_t* in, size_t len, uint8_t out[32]);

// Hash the header's first 64-byte block into a midstate, once per job.
void minerMidstate(const uint8_t block1[64], uint32_t state[8]);

// The per-nonce software double hash: resume from the midstate over the second
// 64-byte block, then hash that digest again.
void minerSha256dFromMidstate(const uint32_t midstate[8], const uint8_t block2[64],
                              uint8_t out[32]);

// Expand compact nbits ("1a44b9f2") into a 32-byte target laid out
// little-endian, i.e. the byte order the double-SHA output comes back in.
bool minerTargetFromNbits(const char* nbits, uint8_t targetLE[32]);

// hash <= target, both little-endian.
bool minerHashMeetsTarget(const uint8_t* hash, const uint8_t targetLE[32]);

// Share difficulty of a hash (difficulty-1 target divided by the hash value).
double minerDiffFromHash(const uint8_t* hash);

// Assemble the block header for one job and precompute midstate + bake.
// All string arguments are NUL-terminated hex exactly as stratum sends them;
// `merkle` is merkleCount raw 32-byte branches. Returns false on malformed
// input (wrong hex lengths, oversized coinbase, bad nbits).
bool minerBuildWork(const char* version, const char* prevHash,
                    const char* coinb1, const char* extranonce1,
                    const char* extranonce2, const char* coinb2,
                    const uint8_t (*merkle)[32], uint8_t merkleCount,
                    const char* ntime, const char* nbits,
                    MinerWork& out);

#endif  // WITH_MINER
