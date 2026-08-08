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
  volatile uint32_t* reg = (volatile uint32_t*)(SHA_TEXT_BASE);
  for (int i = 0; i < 16; i++) reg[i] = w[i];
}

// Second block: words 0-2 are the merkle tail / ntime / nbits, word 3 is the
// nonce, and the rest is SHA padding for an 80-byte message (640 bits).
static inline __attribute__((always_inline)) void hwFillSecondBlock(const void* text, uint32_t nonce) {
  const uint32_t* w = (const uint32_t*)text;
  volatile uint32_t* reg = (volatile uint32_t*)(SHA_TEXT_BASE);
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
  volatile uint32_t* reg = (volatile uint32_t*)(SHA_TEXT_BASE);
  reg[8]  = 0x80000000;
  reg[9]  = 0; reg[10] = 0; reg[11] = 0;
  reg[12] = 0; reg[13] = 0; reg[14] = 0;
  reg[15] = 0x00000100;
}

static inline __attribute__((always_inline)) void hwWaitIdle() {
  while (DPORT_REG_READ(SHA_256_BUSY_REG)) {}
}

// Same, but gives up. Benchmark variants use this: a spin that is too short can
// leave the engine out of step, and an unbounded poll would then spin forever at
// high priority on core 0 and take the watchdog down with it. The bound is far
// above any legitimate block time (~72 cycles).
static inline __attribute__((always_inline)) bool hwWaitIdleBounded() {
  for (uint32_t i = 0; i < 20000; i++)
    if (!DPORT_REG_READ(SHA_256_BUSY_REG)) return true;
  return false;
}

// Read the digest, byte-swapping into the standard SHA-256 output order.
// The `if` variant first checks word 7's low half: the last two digest bytes
// are zero only when it is, which is the cheap 16-bit early exit.
template <bool kFastProbe = false>
static inline __attribute__((always_inline)) bool hwReadDigestSwapped(void* out, bool earlyExit) {
  if (kFastProbe && earlyExit) {
    // Cheap probe first, outside the DPORT workaround. Worst case a mis-read
    // sends us into the full sequence read below, which is still correct.
    if ((_DPORT_REG_READ(SHA_TEXT_BASE + 7 * 4) & 0xFFFF) != 0) return false;
  }
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
  volatile uint32_t* reg = (volatile uint32_t*)(SHA_TEXT_BASE);
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

template <uint32_t kCycles, bool kBounded>
static inline __attribute__((always_inline)) void hwWait() {
  if (kCycles == 0) { if (kBounded) hwWaitIdleBounded(); else hwWaitIdle(); }
  else hwSpinCycles(kCycles);
}

// kFewWrites     — two-write double block instead of eight
// kSkipFinalWait — drop the idle wait after the last load
// kBlockWait     — cycles to spin after a block compression (0 = poll DPORT)
// kLoadWait      — cycles to spin after a digest load     (0 = poll DPORT)
template <bool kFewWrites, bool kSkipFinalWait,
          uint32_t kBlockWait = 0, uint32_t kLoadWait = 0, bool kBounded = false,
          bool kFastProbe = false, bool kOverlap = false>
static inline __attribute__((always_inline)) bool hwDoubleHashT(const uint8_t* swapped128, uint32_t nonce,
                                uint8_t hash[32], bool earlyExit) {
  hwFillFirstBlock(swapped128);
  sha_ll_start_block(SHA2_256);

  // Block 2's fill, hidden inside block 1's compression when overlapping.
  if (kOverlap) hwFillSecondBlock(swapped128 + 64, nonce);
  hwWait<kBlockWait, kBounded>();
  if (!kOverlap) hwFillSecondBlock(swapped128 + 64, nonce);
  sha_ll_continue_block(SHA2_256);

  // The double block only touches words 8 and 15, which the digest load (words
  // 0..7) never disturbs — so it can also be written during the compression.
  if (kOverlap) hwFillDoubleBlockFew();
  hwWait<kBlockWait, kBounded>();
  sha_ll_load(SHA2_256);

  hwWait<kLoadWait, kBounded>();
  if (!kOverlap) { if (kFewWrites) hwFillDoubleBlockFew(); else hwFillDoubleBlock(); }
  sha_ll_start_block(SHA2_256);

  hwWait<kBlockWait, kBounded>();
  sha_ll_load(SHA2_256);
  if (!kSkipFinalWait) hwWait<kLoadWait, kBounded>();
  return hwReadDigestSwapped<kFastProbe>(hash, earlyExit);
}

// The production loop: the fastest variant that stays digest-correct. Measured
// on the NM-TV-154 at 356 KH/s against 323 for the original, checked over 64
// nonces against the software implementation.
static inline __attribute__((always_inline)) bool hwDoubleHash(const uint8_t* swapped128, uint32_t nonce,
                                uint8_t hash[32], bool earlyExit) {
  return hwDoubleHashT<true, true>(swapped128, nonce, hash, earlyExit);
}

// The two-write double block assumes TEXT[9..14] are still zero from the
// previous nonce. mbedTLS shares this engine for the other modes' HTTPS, so
// after every lock acquisition one full-write pass must re-establish that
// state — otherwise the first nonce of each 256-nonce batch could hash wrong.
void minerHwPrime(const uint8_t* swapped128) {
  uint8_t scratch[32];
  hwDoubleHashT<false, false>(swapped128, 0, scratch, false);
}

// ---------------------------------------------------------------------------
void minerHwSwapHeader(const uint8_t* header128, uint8_t* swappedOut128) {
  const uint32_t* in = (const uint32_t*)header128;
  uint32_t* out = (uint32_t*)swappedOut128;
  for (int i = 0; i < 32; i++) out[i] = __builtin_bswap32(in[i]);
}

void minerHwLock()   { esp_sha_lock_engine(SHA2_256); }
bool minerHwTryLock() { return esp_sha_try_lock_engine(SHA2_256); }
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
#define HW_VARIANT(fn, few, skipwait, bwait, lwait, probe, overlap)             \
  __attribute__((noinline)) static bool fn(                                     \
      const uint8_t* s, uint32_t n, uint8_t* h, bool early) {                   \
    return hwDoubleHashT<few, skipwait, bwait, lwait, true, probe, overlap>(s, n, h, early); \
  }

// Round 2. The first pass moved the block wait and the load wait together, so
// when every spin variant came back wrong it could not say which one was short.
// 130/40 was both WRONG and slower than polling — meaning 130 cycles overshoots
// the block time while 40 undershoots the load. So these hold the load wait at
// a poll and vary only the block spin, then do the reverse.
//
// Round 3. The cycle profile changed the target: 16 register writes cost 123
// cycles against 91 for the compression they feed, so the fills dominate, not
// the waits. The overlap variants move each fill inside the previous
// compression, which only works if the engine latches TEXT at start — the
// digest check decides that.
//
// A safe spin is also included: 95 cycles is above the measured 91, whereas the
// 72 that won last round is below it and only survives because the writes that
// follow cover the difference. That is timing-marginal, so it is measured
// against a by-construction-safe value rather than adopted on one reading.
//
//          name            few    skipwait  blockWait  loadWait  probe  overlap
HW_VARIANT(hwVarProd,       true,  true,     0,   0,    false, false)  // current production
HW_VARIANT(hwVarSpin95,     true,  true,     95,  0,    false, false)  // spin >= measured block
HW_VARIANT(hwVarSpin72,     true,  true,     72,  0,    false, false)  // last round's winner
HW_VARIANT(hwVarOver,       true,  true,     0,   0,    false, true)   // overlapped fills
HW_VARIANT(hwVarOverSpin,   true,  true,     95,  0,    false, true)   // overlap + safe spin
HW_VARIANT(hwVarOverProbe,  true,  true,     95,  0,    true,  true)   // + cheap probe

typedef bool (*HwVariantFn)(const uint8_t*, uint32_t, uint8_t*, bool);

// Runs pinned to core 0 — the core the production hardware worker uses — so
// placement and contention effects reproduce instead of being measured away.
// (The first version ran in the web-server task on core 1, time-slicing against
// the software worker there, and reported about half the real rate.)
//
// It does NOT suspend the hash workers. An earlier version did, and since the
// core-0 worker holds the SHA engine mutex almost continuously, suspending it
// froze that mutex locked and deadlocked the device. Instead the bench simply
// takes the engine the ordinary way: the worker releases it every 256 nonces,
// then blocks on it for the duration and resumes on its own.
struct BenchCtx {
  MinerHwBench* out;
  int  maxOut;
  int  count;
  bool acquired;
  volatile bool finished;
  SemaphoreHandle_t done;
};

// File-scope, not on the caller's stack: if the wait below ever times out, the
// bench task must still have somewhere valid to write.
static BenchCtx s_bench;

// Measure what one nonce is actually made of, in CPU cycles. This is the number
// that decides whether a given hashrate is physically reachable at all: three
// engine block times per nonce is the floor, and everything the CPU does sits
// on top of it. Must be called with the engine held.
static void profileEngine(const uint8_t* swapped, MinerHwProfile& p) {
  const uint32_t N = 2000;
  uint32_t t0;

  // 1. The 16 register writes of a block fill, with the engine idle.
  t0 = esp_cpu_get_cycle_count();
  for (uint32_t k = 0; k < N; k++) hwFillFirstBlock(swapped);
  p.writes16 = (esp_cpu_get_cycle_count() - t0) / N;

  // 2. Fill + start + wait for the compression to finish.
  t0 = esp_cpu_get_cycle_count();
  for (uint32_t k = 0; k < N; k++) {
    hwFillFirstBlock(swapped);
    sha_ll_start_block(SHA2_256);
    hwWaitIdleBounded();
  }
  p.blockFull = (esp_cpu_get_cycle_count() - t0) / N;

  // 3. A digest load and its wait.
  t0 = esp_cpu_get_cycle_count();
  for (uint32_t k = 0; k < N; k++) {
    sha_ll_load(SHA2_256);
    hwWaitIdleBounded();
  }
  p.loadPoll = (esp_cpu_get_cycle_count() - t0) / N;

  // 4. The per-nonce early-exit digest probe.
  uint8_t sink[32];
  t0 = esp_cpu_get_cycle_count();
  for (uint32_t k = 0; k < N; k++) hwReadDigestSwapped<false>(sink, true);
  p.probe = (esp_cpu_get_cycle_count() - t0) / N;

  p.engineBlock = (p.blockFull > p.writes16) ? p.blockFull - p.writes16 : 0;
  p.cpuMHz = (uint32_t)(ets_get_cpu_frequency());
  // Three compressions per nonce is unavoidable on this chip: its engine state
  // cannot be seeded with a midstate, so both header blocks are redone each time.
  uint32_t perNonce = p.engineBlock * 3;
  p.ceilingKhs = perNonce ? (uint32_t)((uint64_t)p.cpuMHz * 1000ULL / perNonce) : 0;

  Serial.printf("[miner] profile: writes16=%lu blockFull=%lu engineBlock=%lu "
                "loadPoll=%lu probe=%lu -> ceiling %lu KH/s at %lu MHz\n",
                (unsigned long)p.writes16, (unsigned long)p.blockFull,
                (unsigned long)p.engineBlock, (unsigned long)p.loadPoll,
                (unsigned long)p.probe, (unsigned long)p.ceilingKhs,
                (unsigned long)p.cpuMHz);
}

static MinerHwProfile s_profile;

void minerHwProfileEngine(MinerHwProfile& out) { out = s_profile; }

static void benchTask(void* arg) {
  BenchCtx* ctx = (BenchCtx*)arg;

  struct { const char* name; HwVariantFn fn; } kVariants[] = {
    {"production",            hwVarProd},
    {"+ safe spin 95",        hwVarSpin95},
    {"+ spin 72 (marginal)",  hwVarSpin72},
    {"+ overlapped fills",    hwVarOver},
    {"+ overlap + spin 95",   hwVarOverSpin},
    {"+ overlap+spin+probe",  hwVarOverProbe},
  };
  const int count = (int)(sizeof(kVariants) / sizeof(kVariants[0]));
  const uint32_t kIters = 10000;
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

  // Take the engine without ever blocking indefinitely. The worker releases it
  // every 256 nonces, so this normally succeeds on the first or second try.
  ctx->acquired = false;
  for (int i = 0; i < 200 && !ctx->acquired; i++) {
    if (minerHwTryLock()) ctx->acquired = true;
    else vTaskDelay(pdMS_TO_TICKS(10));
  }
  if (!ctx->acquired) {
    Serial.println("[miner] bench could not take the SHA engine");
    ctx->count = 0;
    ctx->finished = true;
    xSemaphoreGive(ctx->done);
    vTaskDelete(NULL);
    return;
  }

  profileEngine(swapped, s_profile);

  int n = 0;
  for (int i = 0; i < count && i < ctx->maxOut; i++) {
    HwVariantFn fn = kVariants[i].fn;

    // Correctness first, over many nonces: a spin a few cycles short fails
    // intermittently, so checking one nonce would wave it through.
    bool correct = true;
    for (uint32_t k = 0; k < kCheckIters; k++) {
      uint32_t nonce = 0x9546a142 + k;
      uint8_t want[32], got[32];
      memcpy(header + 76, &nonce, 4);
      minerSha256d(header, 80, want);
      fn(swapped, nonce, got, false);
      if (memcmp(want, got, 32) != 0) { correct = false; break; }
    }

    uint8_t sink[32];
    uint32_t best = 0;
    for (int rep = 0; rep < 3; rep++) {
      uint32_t t0 = micros();
      for (uint32_t k = 0; k < kIters; k++)
        fn(swapped, 0x10000000 + k, sink, true);
      uint32_t dt = micros() - t0;
      uint32_t khs = dt ? (uint32_t)((uint64_t)kIters * 1000ULL / dt) : 0;
      if (khs > best) best = khs;
    }

    ctx->out[n].name    = kVariants[i].name;
    ctx->out[n].khs     = best;
    ctx->out[n].correct = correct;
    Serial.printf("[miner] bench %-22s %6lu KH/s  %s\n", ctx->out[n].name,
                  (unsigned long)ctx->out[n].khs, correct ? "ok" : "WRONG DIGEST");
    n++;

    // Leave the engine settled before the next variant, and hand core 0 back
    // briefly so the idle task can feed the watchdog. If a variant ever wedges
    // the engine, take the hardware path out of service rather than let the
    // production loop — which waits unbounded — hang on it when mining resumes.
    if (!hwWaitIdleBounded()) {
      minerCoreReportHwFault("engine did not return to idle after benchmark");
      break;
    }
    vTaskDelay(1);
  }

  minerHwUnlock();
  ctx->count = n;
  ctx->finished = true;
  xSemaphoreGive(ctx->done);
  vTaskDelete(NULL);
}

int minerHwBenchmark(MinerHwBench* out, int maxOut) {
  if (s_bench.done && !s_bench.finished) return 0;   // one at a time

  s_bench.out      = out;
  s_bench.maxOut   = maxOut;
  s_bench.count    = 0;
  s_bench.acquired = false;
  s_bench.finished = false;
  if (!s_bench.done) s_bench.done = xSemaphoreCreateBinary();
  if (!s_bench.done) return 0;

  TaskHandle_t t = nullptr;
  if (xTaskCreatePinnedToCore(benchTask, "miner-bench", 6144, &s_bench, 3, &t, 0) != pdPASS)
    return 0;

  // Bounded: the web server must come back even if the bench somehow wedges.
  if (xSemaphoreTake(s_bench.done, pdMS_TO_TICKS(15000)) != pdTRUE) {
    Serial.println("[miner] bench timed out");
    return 0;
  }
  return s_bench.count;
}

#endif  // MINER_HAS_SHA_HW
