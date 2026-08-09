#include "MinerShaHw.h"
#if MINER_HAS_SHA_HW

#include <soc/hwcrypto_reg.h>
#include <soc/dport_access.h>
#include <hal/sha_ll.h>
#include <sha/sha_parallel_engine.h>
#include <esp_cpu.h>

// What this loop looks like was decided by measurement on an NM-TV-154, not by
// reasoning. The numbers, at 240 MHz, per nonce:
//
//   16 register writes     106-123 cyc      engine block compression  86-94 cyc
//   digest load + poll          52 cyc      early-exit probe             42 cyc
//
// Feeding the engine costs about as much as the engine. Two 16-word fills plus
// the small double-block fill is ~261 cycles against ~276 for three
// compressions, so the floor is ~537 cycles/nonce — about 447 KH/s — and even
// with the writes free it would be 869 KH/s. This loop measures 347 KH/s.
//
// Things that were tried and rejected, recorded so they are not tried again:
//
//   IRAM placement     350 -> 280 KH/s. Slower, measured twice, and restoring
//                      inlining did not recover it. The per-core instruction
//                      cache serves this fine from flash.
//   Overlapped fills   Wrong digest, every variant. This engine reads TEXT
//                      progressively during a compression rather than latching
//                      it at start_block, so a fill cannot hide inside the
//                      previous block. That is what sets the floor above.
//   Fast DPORT reads   For the *full* digest. Reading all eight words with
//                      DPORT_SEQUENCE_REG_READ returns another master's data in
//                      the first read or two under load from the other core. The
//                      device showed it directly: a bad digest and an immediate
//                      re-read agreed on words 1-6 and disagreed on 0 and 7 —
//                      the two read earliest. Word 7 corrupting to zero was the
//                      damaging case, since zero is precisely what the early
//                      exit is looking for, so the engine's read errors were
//                      selected *for* rather than filtered out. The full read
//                      now uses the protected accessor; the probe does not,
//                      because a wrong probe only wastes a full read.
//   Bare fixed spin    Any spin with no poll behind it, including the 95 that
//                      shipped. The reasoning against spin 72 — undershoot is a
//                      silently wrong hash — applied to 95 as well, just more
//                      rarely, and the device proved it: a digest read across
//                      the final load came back zeroed, and a zero word 7 walks
//                      straight through the early-exit test, so the engine's
//                      failures were biased *toward* looking like shares rather
//                      than being filtered out as noise. Spins now always have
//                      a busy-register poll behind them, which is also what let
//                      the spin drop back to 72.
//
// On the stock NMMiner firmware's claimed 1035 KH/s: three compressions per
// nonce are forced here, since the classic ESP32's engine state cannot be seeded
// with a midstate, so 1035 sits 19% above the ceiling this silicon has even with
// free register writes. It is most consistent with counting SHA-256 compressions
// rather than nonce attempts — 388 KH/s of nonces is 1164 K compressions/s.

// MMIO must be volatile: block 1 is identical for every nonce in a job, so
// nothing else stops the compiler hoisting those sixteen writes out of the hot
// loop and feeding the engine stale data.
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

// The second hash needs padding for a 32-byte message. After the first hash's
// load, TEXT[0..7] hold the digest and TEXT[8..15] still hold what the second
// block left there — zeros through word 14 — so only two writes are needed.
// That relies on load() not touching TEXT[8..15], which held across 64-nonce
// digest checks, and on the engine state being ours: see minerHwPrime().
static inline __attribute__((always_inline)) void hwFillDoubleBlock() {
  volatile uint32_t* reg = (volatile uint32_t*)(SHA_TEXT_BASE);
  reg[8]  = 0x80000000;
  reg[15] = 0x00000100;
}

static inline __attribute__((always_inline)) void hwFillDoubleBlockFull() {
  volatile uint32_t* reg = (volatile uint32_t*)(SHA_TEXT_BASE);
  reg[8]  = 0x80000000;
  reg[9]  = 0; reg[10] = 0; reg[11] = 0;
  reg[12] = 0; reg[13] = 0; reg[14] = 0;
  reg[15] = 0x00000100;
}

// The spin covers most of the engine's latency without bus traffic (CCOUNT is
// an internal register), and a busy-register poll behind it supplies the rest.
// It used to stand alone, sized at 95 to sit above the measured 86-94 cycle
// compression — but "above the range I measured" is not a guarantee on a chip
// whose timing moves with bus contention, and undershooting is silent: it reads
// the digest early and the wrong hash looks exactly like the right one.
//
// With a poll behind it the spin no longer has to be safe, only close, so it
// goes back to the 72 that measured faster and was rejected for having no
// margin. The poll is now where the margin lives.
static inline __attribute__((always_inline)) void hwSpin(uint32_t n) {
  uint32_t t0 = esp_cpu_get_cycle_count();
  while ((esp_cpu_get_cycle_count() - t0) < n) {}
}
#define HW_BLOCK_SPIN 72

static inline __attribute__((always_inline)) void hwWaitIdle() {
  while (DPORT_REG_READ(SHA_256_BUSY_REG)) {}
}

// Read the digest, byte-swapping into standard SHA-256 output order.
//
// Two accessors, deliberately. The device settled which is needed where: a
// mismatching digest was captured alongside an immediate re-read of the same
// registers, and words 1-6 were identical across both while words 0 and 7
// differed. Those two are exactly the first two reads this function performs.
// The fast DPORT sequence accessor is only sound once the access pattern is
// established; its first reads can return another bus master's data, and core 1
// is running a hash worker and WiFi the whole time.
//
// So the per-nonce probe keeps the fast read — a bad value there costs at most
// a wasted full read, which the recheck catches — and the full digest, which
// happens about once in 65536 nonces, uses the protected accessor that is
// correct under contention and far too expensive to put in the hot path.
static inline __attribute__((always_inline)) bool hwReadDigest(void* out, bool earlyExit) {
  if (earlyExit) {
    DPORT_INTERRUPT_DISABLE();
    const uint32_t last = DPORT_SEQUENCE_REG_READ(SHA_TEXT_BASE + 7 * 4);
    DPORT_INTERRUPT_RESTORE();
    if ((last & 0xFFFF) != 0) return false;
  }
  uint32_t* p = (uint32_t*)out;
  for (int i = 0; i < 8; i++)
    p[i] = __builtin_bswap32(DPORT_REG_READ(SHA_TEXT_BASE + i * 4));
  return true;
}

// One nonce. kPrime writes the double block in full and polls rather than
// spinning: used once after taking the engine, to establish the TEXT state the
// fast path assumes.
template <bool kPrime>
static inline __attribute__((always_inline)) bool hwDoubleHash(const uint8_t* swapped128, uint32_t nonce,
                                uint8_t hash[32], bool earlyExit) {
  hwFillFirstBlock(swapped128);
  sha_ll_start_block(SHA2_256);

  if (!kPrime) hwSpin(HW_BLOCK_SPIN);
  hwWaitIdle();
  hwFillSecondBlock(swapped128 + 64, nonce);
  sha_ll_continue_block(SHA2_256);

  if (!kPrime) hwSpin(HW_BLOCK_SPIN);
  hwWaitIdle();
  sha_ll_load(SHA2_256);

  hwWaitIdle();
  if (kPrime) hwFillDoubleBlockFull(); else hwFillDoubleBlock();
  sha_ll_start_block(SHA2_256);

  if (!kPrime) hwSpin(HW_BLOCK_SPIN);
  hwWaitIdle();
  sha_ll_load(SHA2_256);
  // The load moves the digest into TEXT and is not instant. Reading across it
  // returns zeros, and a zero word 7 walks straight through the early exit —
  // which is how a bad digest ends up looking like a share worth submitting.
  hwWaitIdle();
  return hwReadDigest(hash, earlyExit);
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
  return hwDoubleHash<false>(swapped128, nonce, hash, /*earlyExit=*/true);
}

void minerHwSha256dRaw(const uint8_t* swapped128, uint32_t nonce, uint8_t hash[32]) {
  hwDoubleHash<true>(swapped128, nonce, hash, /*earlyExit=*/false);
}

// mbedTLS shares this engine for the other modes' HTTPS, so after every lock
// acquisition one full pass re-establishes the TEXT state the two-write double
// block depends on. Without it the first nonce of each batch could hash wrong.
void minerHwPrime(const uint8_t* swapped128) {
  uint8_t scratch[32];
  hwDoubleHash<true>(swapped128, 0, scratch, false);
}

#endif  // MINER_HAS_SHA_HW
