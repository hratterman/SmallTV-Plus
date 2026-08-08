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
bool minerHwTryLock();   // never blocks; the benchmark must not wait forever
void minerHwUnlock();

// One nonce. Returns false on the same 16-bit early exit the software path uses
// (the hash cannot meet any share target), leaving `hash` untouched.
bool minerHwSha256d(const uint8_t* swapped128, uint32_t nonce, uint8_t hash[32]);

// Re-establish the engine state the fast path depends on. Must be called once
// after each minerHwLock(), since mbedTLS may have used the engine in between.
void minerHwPrime(const uint8_t* swapped128);

// Same, but always reads back the digest — used by the startup self-check.
void minerHwSha256dRaw(const uint8_t* swapped128, uint32_t nonce, uint8_t hash[32]);

// ---------------------------------------------------------------------------
// On-device benchmark.
//
// Reflashing to test one variant at a time is a ~10 minute round trip, which is
// no way to close a 3x gap. This times every candidate driving loop back to back
// on the real chip in well under a second, and checks each one's digest against
// the software implementation first — so a variant that is fast because it is
// wrong reports correct=false instead of quietly mining garbage.
//
// It runs on a fixed header (mainnet block 125552), so results are comparable
// across runs and do not need the miner to be connected to a pool.
struct MinerHwBench {
  const char* name;
  uint32_t    khs;      // measured, this variant alone
  bool        correct;  // digest matched the software reference
};

// Fills up to maxOut entries, returns how many were written. Takes the SHA
// engine for the duration (the mining worker simply waits).
int minerHwBenchmark(MinerHwBench* out, int maxOut);

// Cycle-level profile of the pieces of one nonce, so the ceiling can be
// computed from what this engine actually does rather than from round counts.
// Everything is average CPU cycles at the current clock.
struct MinerHwProfile {
  uint32_t writes16;     // the 16 register writes of a block fill, alone
  uint32_t blockFull;    // fill + start_block + poll until idle
  uint32_t loadPoll;     // load + poll until idle
  uint32_t probe;        // the early-exit digest read
  uint32_t engineBlock;  // blockFull - writes16: the engine's own block time
  uint32_t cpuMHz;
  uint32_t ceilingKhs;   // 3 engine blocks per nonce, zero CPU overhead
};
void minerHwProfileEngine(MinerHwProfile& out);

// Clock scaling: the same loop measured at each supported CPU frequency. This
// answers whether overclocking would buy anything — parts on the CPU clock keep
// a constant cycle count as the clock rises, parts on the fixed peripheral bus
// cost proportionally MORE cycles for the same wall-clock time.
struct MinerHwClock {
  uint32_t mhz;
  uint32_t khs;          // production loop at that clock
  uint32_t writes16;     // cycles for a 16-word fill
  uint32_t engineBlock;  // cycles for one compression
};
int minerHwClockScan(MinerHwClock* out, int maxOut);

#endif  // MINER_HAS_SHA_HW
