#include "MinerShaHw.h"
#if MINER_HAS_SHA_HW

#include <soc/hwcrypto_reg.h>
#include <soc/dport_access.h>
#include <hal/sha_ll.h>
#include <sha/sha_parallel_engine.h>
#include <esp_cpu.h>

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
// Waiting by polling SHA_256_BUSY_REG costs a DPORT read that stalls the CPU,
// and we do it six times per nonce. The engine's block time is deterministic, so
// the alternative is to spin a fixed number of CPU cycles on CCOUNT — an
// internal register read, no bus traffic. Too short a spin reads the digest
// early and produces a wrong hash, which is exactly what the benchmark's digest
// check is there to catch.
static inline __attribute__((always_inline)) void hwSpinCycles(uint32_t n) {
  uint32_t t0 = esp_cpu_get_cycle_count();
  while ((esp_cpu_get_cycle_count() - t0) < n) {}
}

template <uint32_t kCycles>
static inline __attribute__((always_inline)) void hwWait() {
  if (kCycles == 0) hwWaitIdle(); else hwSpinCycles(kCycles);
}

// kFewWrites     — two-write double block instead of eight
// kSkipFinalWait — drop the idle wait after the last load
// kBlockWait     — cycles to spin after a block compression (0 = poll DPORT)
// kLoadWait      — cycles to spin after a digest load     (0 = poll DPORT)
template <bool kFewWrites, bool kSkipFinalWait,
          uint32_t kBlockWait = 0, uint32_t kLoadWait = 0>
static inline __attribute__((always_inline)) bool hwDoubleHashT(const uint8_t* swapped128, uint32_t nonce,
                                uint8_t hash[32], bool earlyExit) {
  hwFillFirstBlock(swapped128);
  sha_ll_start_block(SHA2_256);

  hwWait<kBlockWait>();
  hwFillSecondBlock(swapped128 + 64, nonce);
  sha_ll_continue_block(SHA2_256);

  hwWait<kBlockWait>();
  sha_ll_load(SHA2_256);

  hwWait<kLoadWait>();
  if (kFewWrites) hwFillDoubleBlockFew(); else hwFillDoubleBlock();
  sha_ll_start_block(SHA2_256);

  hwWait<kBlockWait>();
  sha_ll_load(SHA2_256);
  if (!kSkipFinalWait) hwWait<kLoadWait>();
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
#include "MinerCore.h"

// Each candidate is its own noinline function so the timing loop cannot inline
// one and not another, and so the IRAM copy really executes from IRAM. They all
// pay the same one call per nonce, which keeps the comparison fair (and makes
// the absolute numbers a shade pessimistic against production).
#define HW_VARIANT(fn, few, skipwait, bwait, lwait)                             \
  __attribute__((noinline)) static bool fn(                                     \
      const uint8_t* s, uint32_t n, uint8_t* h, bool early) {                   \
    return hwDoubleHashT<few, skipwait, bwait, lwait>(s, n, h, early);          \
  }

//          name                few    skipwait  blockWait  loadWait
HW_VARIANT(hwVarBase,           false, false,    0,   0)   // today's production loop
HW_VARIANT(hwVarWrites,         true,  true,     0,   0)   // fewer writes, no final wait
HW_VARIANT(hwVarSpin130,        true,  true,     130, 40)  // conservative fixed spin
HW_VARIANT(hwVarSpin100,        true,  true,     100, 30)
HW_VARIANT(hwVarSpin85,         true,  true,     85,  20)
HW_VARIANT(hwVarSpin72,         true,  true,     72,  12)  // ~the engine's block time

typedef bool (*HwVariantFn)(const uint8_t*, uint32_t, uint8_t*, bool);

// Runs on core 0 with the hash workers suspended: the first version of this
// benchmark ran in the web-server task on core 1, time-slicing against the
// software worker there, and so reported about half the real rate and ranked
// IRAM the opposite way round from production.
struct BenchCtx {
  MinerHwBench* out;
  int maxOut;
  int count;
  SemaphoreHandle_t done;
};

static void benchTask(void* arg) {
  BenchCtx* ctx = (BenchCtx*)arg;

  struct { const char* name; HwVariantFn fn; } kVariants[] = {
    {"base (production)",     hwVarBase},
    {"fewer writes+no wait",  hwVarWrites},
    {"fixed spin 130/40",     hwVarSpin130},
    {"fixed spin 100/30",     hwVarSpin100},
    {"fixed spin 85/20",      hwVarSpin85},
    {"fixed spin 72/12",      hwVarSpin72},
  };
  const int count = (int)(sizeof(kVariants) / sizeof(kVariants[0]));
  const uint32_t kIters = 20000;
  const uint32_t kCheckIters = 64;

  // Mainnet block 125552's header: a known-good reference, and it keeps results
  // comparable between runs without needing a pool connection.
  static const char kHeaderHex[] =
      "01000000"
      "81cd02ab7e569e8bcd9317e2fe99f2de44d49ab2b8851ba4a308000000000000"
      "e320b6c2fffc8d750423db8b1eb942ae710e951ed797f7affc8892b0f1fc122b"
      "c7f5d74d" "f2b9441a" "00000000";

  uint8_t header[128];
  memset(header, 0, sizeof(header));
  minerHexToBytes(kHeaderHex, sizeof(kHeaderHex) - 1, header);
  header[80] = 0x80; header[126] = 0x02; header[127] = 0x80;

  uint8_t swapped[128];
  minerHwSwapHeader(header, swapped);

  int n = 0;
  minerHwLock();
  for (int i = 0; i < count && i < ctx->maxOut; i++) {
    HwVariantFn fn = kVariants[i].fn;

    // Correctness first, over many nonces: a spin that is a few cycles short
    // fails intermittently, so checking a single nonce would miss it.
    bool correct = true;
    for (uint32_t k = 0; k < kCheckIters; k++) {
      uint32_t nonce = 0x9546a142 + k;
      uint8_t want[32], got[32];
      memcpy(header + 76, &nonce, 4);
      minerSha256d(header, 80, want);
      fn(swapped, nonce, got, false);
      if (memcmp(want, got, 32) != 0) { correct = false; break; }
    }

    uint32_t t0 = micros();
    uint8_t sink[32];
    for (uint32_t k = 0; k < kIters; k++)
      fn(swapped, 0x10000000 + k, sink, true);
    uint32_t dt = micros() - t0;

    ctx->out[n].name    = kVariants[i].name;
    ctx->out[n].khs     = dt ? (uint32_t)((uint64_t)kIters * 1000ULL / dt) : 0;
    ctx->out[n].correct = correct;
    Serial.printf("[miner] bench %-22s %6lu KH/s  %s\n", ctx->out[n].name,
                  (unsigned long)ctx->out[n].khs, correct ? "ok" : "WRONG DIGEST");
    n++;
  }
  minerHwUnlock();

  ctx->count = n;
  xSemaphoreGive(ctx->done);
  vTaskDelete(NULL);
}

int minerHwBenchmark(MinerHwBench* out, int maxOut) {
  BenchCtx ctx = {out, maxOut, 0, xSemaphoreCreateBinary()};
  if (!ctx.done) return 0;

  minerCoreSuspendWorkers();
  TaskHandle_t t = nullptr;
  // Pinned to core 0, the same core the production hardware worker runs on, so
  // placement and contention effects reproduce rather than being measured away.
  if (xTaskCreatePinnedToCore(benchTask, "miner-bench", 4096, &ctx, 3, &t, 0) == pdPASS)
    xSemaphoreTake(ctx.done, portMAX_DELAY);
  minerCoreResumeWorkers();

  vSemaphoreDelete(ctx.done);
  return ctx.count;
}

#endif  // MINER_HAS_SHA_HW
