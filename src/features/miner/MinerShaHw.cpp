#include "MinerShaHw.h"
#if MINER_HAS_SHA_HW

#include <soc/hwcrypto_reg.h>
#include <soc/dport_access.h>
#include <hal/sha_ll.h>
#include <sha/sha_parallel_engine.h>

// Placement note, measured on hardware rather than assumed: putting this loop
// in IRAM cost ~20% (350 -> 280 KH/s) and restoring inlining did not recover it,
// so it lives in flash where the per-core instruction cache serves it. The
// helpers stay always_inline — marking a static inline function with an explicit
// section attribute defeats inlining, which is its own 7-calls-per-nonce trap.
//
// The register poking below is kept close to NerdMiner_v2's original: the
// classic ESP32's DPORT peripheral needs the sequence-read/interrupt-disable
// dance when the other core is active, and that is exactly the kind of detail
// worth copying rather than re-deriving.

static inline __attribute__((always_inline)) void hwFillFirstBlock(const void* text) {
  const uint32_t* w = (const uint32_t*)text;
  uint32_t* reg = (uint32_t*)(SHA_TEXT_BASE);
  for (int i = 0; i < 16; i++) reg[i] = w[i];
}

// Second block: words 0-2 are the merkle tail / ntime / nbits, word 3 is the
// nonce, and the rest is SHA padding for an 80-byte message (640 bits).
static inline __attribute__((always_inline)) void hwFillSecondBlock(const void* text, uint32_t nonce) {
  const uint32_t* w = (const uint32_t*)text;
  uint32_t* reg = (uint32_t*)(SHA_TEXT_BASE);
  reg[0]  = w[0];
  reg[1]  = w[1];
  reg[2]  = w[2];
  reg[3]  = __builtin_bswap32(nonce);
  reg[4]  = 0x80000000;
  reg[5]  = 0; reg[6]  = 0; reg[7]  = 0;
  reg[8]  = 0; reg[9]  = 0; reg[10] = 0; reg[11] = 0;
  reg[12] = 0; reg[13] = 0; reg[14] = 0;
  reg[15] = 0x00000280;
}

// Second hash: the first digest is already sitting in words 0-7 after a load,
// so only the padding for a 32-byte message (256 bits) needs writing.
static inline __attribute__((always_inline)) void hwFillDoubleBlock() {
  uint32_t* reg = (uint32_t*)(SHA_TEXT_BASE);
  reg[8]  = 0x80000000;
  reg[9]  = 0; reg[10] = 0; reg[11] = 0;
  reg[12] = 0; reg[13] = 0; reg[14] = 0;
  reg[15] = 0x00000100;
}

static inline __attribute__((always_inline)) void hwWaitIdle() {
  while (DPORT_REG_READ(SHA_256_BUSY_REG)) {}
}

// Read the digest, byte-swapping into the standard SHA-256 output order.
// The `if` variant first checks word 7's low half: the last two digest bytes
// are zero only when it is, which is the cheap 16-bit early exit.
static inline __attribute__((always_inline)) bool hwReadDigestSwapped(void* out, bool earlyExit) {
  DPORT_INTERRUPT_DISABLE();
  uint32_t last = DPORT_SEQUENCE_REG_READ(SHA_TEXT_BASE + 7 * 4);
  if (earlyExit && (last & 0xFFFF) != 0) {
    DPORT_INTERRUPT_RESTORE();
    return false;
  }
  uint32_t* p = (uint32_t*)out;
  p[7] = __builtin_bswap32(last);
  p[0] = __builtin_bswap32(DPORT_SEQUENCE_REG_READ(SHA_TEXT_BASE + 0 * 4));
  p[1] = __builtin_bswap32(DPORT_SEQUENCE_REG_READ(SHA_TEXT_BASE + 1 * 4));
  p[2] = __builtin_bswap32(DPORT_SEQUENCE_REG_READ(SHA_TEXT_BASE + 2 * 4));
  p[3] = __builtin_bswap32(DPORT_SEQUENCE_REG_READ(SHA_TEXT_BASE + 3 * 4));
  p[4] = __builtin_bswap32(DPORT_SEQUENCE_REG_READ(SHA_TEXT_BASE + 4 * 4));
  p[5] = __builtin_bswap32(DPORT_SEQUENCE_REG_READ(SHA_TEXT_BASE + 5 * 4));
  p[6] = __builtin_bswap32(DPORT_SEQUENCE_REG_READ(SHA_TEXT_BASE + 6 * 4));
  DPORT_INTERRUPT_RESTORE();
  return true;
}

// Cheaper double-block fill. After the first hash's load, TEXT[0..7] hold the
// digest and TEXT[8..15] still hold what the second block left there — which was
// 0x80000000 at word 4 and zeros through word 14. So words 9..14 are already
// zero and only two writes are actually needed. Saves 6 of the 40 register
// writes per nonce; correctness depends on load() not touching TEXT[8..15],
// which is exactly what the benchmark's digest check verifies.
static inline __attribute__((always_inline)) void hwFillDoubleBlockFew() {
  uint32_t* reg = (uint32_t*)(SHA_TEXT_BASE);
  reg[8]  = 0x80000000;
  reg[15] = 0x00000100;
}

// The whole two-block double hash for one nonce.
//   kFewWrites     — use the two-write double block above
//   kSkipFinalWait — drop the idle wait after the last load (NerdMiner's
//                    original omits it; I added it defensively)
template <bool kFewWrites, bool kSkipFinalWait>
static inline __attribute__((always_inline)) bool hwDoubleHashT(const uint8_t* swapped128, uint32_t nonce,
                                uint8_t hash[32], bool earlyExit) {
  hwFillFirstBlock(swapped128);
  sha_ll_start_block(SHA2_256);

  hwWaitIdle();
  hwFillSecondBlock(swapped128 + 64, nonce);
  sha_ll_continue_block(SHA2_256);

  hwWaitIdle();
  sha_ll_load(SHA2_256);

  hwWaitIdle();
  if (kFewWrites) hwFillDoubleBlockFew(); else hwFillDoubleBlock();
  sha_ll_start_block(SHA2_256);

  hwWaitIdle();
  sha_ll_load(SHA2_256);
  if (!kSkipFinalWait) hwWaitIdle();
  return hwReadDigestSwapped(hash, earlyExit);
}

// The production loop: the variant that measures fastest goes here.
static inline __attribute__((always_inline)) bool hwDoubleHash(const uint8_t* swapped128, uint32_t nonce,
                                uint8_t hash[32], bool earlyExit) {
  return hwDoubleHashT<false, false>(swapped128, nonce, hash, earlyExit);
}

// ---------------------------------------------------------------------------
void minerHwSwapHeader(const uint8_t* header128, uint8_t* swappedOut128) {
  const uint32_t* in = (const uint32_t*)header128;
  uint32_t* out = (uint32_t*)swappedOut128;
  for (int i = 0; i < 32; i++) out[i] = __builtin_bswap32(in[i]);
}

void minerHwLock()   { esp_sha_lock_engine(SHA2_256); }
void minerHwUnlock() { esp_sha_unlock_engine(SHA2_256); }

bool minerHwSha256d(const uint8_t* swapped128, uint32_t nonce, uint8_t hash[32]) {
  return hwDoubleHash(swapped128, nonce, hash, /*earlyExit=*/true);
}

void minerHwSha256dRaw(const uint8_t* swapped128, uint32_t nonce, uint8_t hash[32]) {
  hwDoubleHash(swapped128, nonce, hash, /*earlyExit=*/false);
}

// ---------------------------------------------------------------------------
// Benchmark
// ---------------------------------------------------------------------------
#include "MinerJob.h"

// Each candidate is its own noinline function so the timing loop cannot inline
// one and not another, and so the IRAM copy really executes from IRAM. They all
// pay the same one call per nonce, which keeps the comparison fair (and makes
// the absolute numbers a shade pessimistic against production).
#define HW_VARIANT(fn, attr, few, skipwait)                                     \
  attr __attribute__((noinline)) static bool fn(                                \
      const uint8_t* s, uint32_t n, uint8_t* h, bool early) {                   \
    return hwDoubleHashT<few, skipwait>(s, n, h, early);                        \
  }

HW_VARIANT(hwVarBase,        ,          false, false)
HW_VARIANT(hwVarBaseIram,    IRAM_ATTR, false, false)
HW_VARIANT(hwVarFewWrites,   ,          true,  false)
HW_VARIANT(hwVarNoFinalWait, ,          false, true)
HW_VARIANT(hwVarBoth,        ,          true,  true)

typedef bool (*HwVariantFn)(const uint8_t*, uint32_t, uint8_t*, bool);

int minerHwBenchmark(MinerHwBench* out, int maxOut) {
  struct { const char* name; HwVariantFn fn; } kVariants[] = {
    {"base (flash)",           hwVarBase},
    {"base (IRAM)",            hwVarBaseIram},
    {"fewer writes",           hwVarFewWrites},
    {"no final wait",          hwVarNoFinalWait},
    {"fewer writes+no wait",   hwVarBoth},
  };
  const int count = (int)(sizeof(kVariants) / sizeof(kVariants[0]));
  const uint32_t kIters = 20000;

  // Mainnet block 125552's header, so the reference digest is a known quantity
  // and results stay comparable between runs.
  static const char kHeaderHex[] =
      "01000000"
      "81cd02ab7e569e8bcd9317e2fe99f2de44d49ab2b8851ba4a308000000000000"
      "e320b6c2fffc8d750423db8b1eb942ae710e951ed797f7affc8892b0f1fc122b"
      "c7f5d74d" "f2b9441a" "00000000";

  uint8_t header[128];
  memset(header, 0, sizeof(header));
  minerHexToBytes(kHeaderHex, sizeof(kHeaderHex) - 1, header);
  header[80] = 0x80; header[126] = 0x02; header[127] = 0x80;

  const uint32_t kCheckNonce = 0x9546a142;      // this block's winning nonce
  memcpy(header + 76, &kCheckNonce, 4);
  uint8_t want[32];
  minerSha256d(header, 80, want);               // software reference

  uint8_t swapped[128];
  minerHwSwapHeader(header, swapped);

  int n = 0;
  minerHwLock();
  for (int i = 0; i < count && i < maxOut; i++) {
    uint8_t got[32];
    // Prime first: the fewer-writes variant depends on state the previous
    // iteration left in the engine's text registers.
    kVariants[i].fn(swapped, kCheckNonce, got, false);
    kVariants[i].fn(swapped, kCheckNonce, got, false);
    bool correct = memcmp(want, got, 32) == 0;

    uint32_t t0 = micros();
    for (uint32_t k = 0; k < kIters; k++)
      kVariants[i].fn(swapped, 0x10000000 + k, got, true);
    uint32_t dt = micros() - t0;

    out[n].name    = kVariants[i].name;
    out[n].khs     = dt ? (uint32_t)((uint64_t)kIters * 1000ULL / dt) : 0;
    out[n].correct = correct;
    Serial.printf("[miner] bench %-22s %6lu KH/s  %s\n", out[n].name,
                  (unsigned long)out[n].khs, correct ? "ok" : "WRONG DIGEST");
    n++;
  }
  minerHwUnlock();
  return n;
}

#endif  // MINER_HAS_SHA_HW
