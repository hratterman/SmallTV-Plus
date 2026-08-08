// MinerShaHw.h — SHA-256d on the classic ESP32's hardware SHA peripheral.
//
// Ported from BitMaker-hub/NerdMiner_v2 src/mining.cpp minerWorkerHw()'s
// CONFIG_IDF_TARGET_ESP32 branch (MIT, (c) 2023 Bitmaker), which drives the
// engine through its registers rather than through mbedTLS.
//
// Worth knowing before assuming this is the fast path: unlike the S2/S3/C3
// engines, the classic ESP32's SHA block cannot be seeded with a precomputed
// midstate, so it must hash *both* header blocks for every nonce — three block
// compressions per nonce against the software path's ~2 (cached midstate plus
// the 16-bit early exit). Which one wins is a measurement, not a deduction,
// which is why the engine is switchable at runtime from the web UI.
//
// The engine is shared with mbedTLS (TLS handshakes for the other modes), so
// callers lock it around a batch and release promptly.
#pragma once
#include "config.h"
// Both ESP32 targets (NM-TV-154 and the 8 MB Pro) are the same classic silicon
// with the same peripheral, so this covers both.
#if WITH_MINER && defined(SMALLTV_ESP32)
#define MINER_HAS_SHA_HW 1
#else
#define MINER_HAS_SHA_HW 0
#endif

#if MINER_HAS_SHA_HW

#include <Arduino.h>
#include <stdint.h>

// The engine reads its input as big-endian words, so the 128-byte padded header
// is pre-swapped once per job into this form.
void minerHwSwapHeader(const uint8_t* header128, uint8_t* swappedOut128);

void minerHwLock();
void minerHwUnlock();

// One nonce. Returns false on the same 16-bit early exit the software path uses
// (the hash cannot meet any share target), leaving `hash` untouched.
bool minerHwSha256d(const uint8_t* swapped128, uint32_t nonce, uint8_t hash[32]);

// Re-establish the engine state the fast path depends on. Must be called once
// after each minerHwLock(), since mbedTLS may have used the engine in between.
void minerHwPrime(const uint8_t* swapped128);

// Same, but always reads back the digest — used by the startup self-check.
void minerHwSha256dRaw(const uint8_t* swapped128, uint32_t nonce, uint8_t hash[32]);

// Re-read the digest registers the engine last produced, waiting for idle and
// taking all eight words with no early exit. Valid only immediately after a
// hash, before the engine is touched again. This is the test that separates a
// bad *read* from a bad *computation*: if a mismatching digest re-reads as the
// value software expected, the engine was right and the first read was not.
void minerHwRereadDigest(uint8_t hash[32]);

#endif  // MINER_HAS_SHA_HW
