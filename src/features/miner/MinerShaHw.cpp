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
//   Fixed spin 72      Faster (388 KH/s) and passed its digest check, but it
//                      waits less than the compression takes and survives only
//                      on a few cycles of loop overhead. The failure mode is a
//                      silently wrong hash — missed shares, invisibly — so it
//                      is not worth 12%.
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

// A fixed spin beats polling the busy register: the poll costs a DPORT read that
// stalls the CPU, several times per nonce, to wait out a delay the engine takes
// deterministically. 95 sits above the measured 86-94 cycle compression rather
// than below it. CCOUNT is an internal register — no bus traffic — and the spin
// only ever overshoots, never undershoots.
static inline __attribute__((always_inline)) void hwSpin(uint32_t n) {
  uint32_t t0 = esp_cpu_get_cycle_count();
  while ((esp_cpu_get_cycle_count() - t0) < n) {}
}
#define HW_BLOCK_SPIN 95

static inline __attribute__((always_inline)) void hwWaitIdle() {
  while (DPORT_REG_READ(SHA_256_BUSY_REG)) {}
}

// Read the digest, byte-swapping into standard SHA-256 output order. The
// early-exit checks word 7's low half first: the last two digest bytes are zero
// only when it is, and all but ~1/65536 nonces stop right there.
static inline __attribute__((always_inline)) bool hwReadDigest(void* out, bool earlyExit) {
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

// One nonce. kPrime writes the double block in full and polls rather than
// spinning: used once after taking the engine, to establish the TEXT state the
// fast path assumes.
template <bool kPrime>
static inline __attribute__((always_inline)) bool hwDoubleHash(const uint8_t* swapped128, uint32_t nonce,
                                uint8_t hash[32], bool earlyExit) {
  hwFillFirstBlock(swapped128);
  sha_ll_start_block(SHA2_256);

  if (kPrime) hwWaitIdle(); else hwSpin(HW_BLOCK_SPIN);
  hwFillSecondBlock(swapped128 + 64, nonce);
  sha_ll_continue_block(SHA2_256);

  if (kPrime) hwWaitIdle(); else hwSpin(HW_BLOCK_SPIN);
  sha_ll_load(SHA2_256);

  hwWaitIdle();
  if (kPrime) hwFillDoubleBlockFull(); else hwFillDoubleBlock();
  sha_ll_start_block(SHA2_256);

  if (kPrime) hwWaitIdle(); else hwSpin(HW_BLOCK_SPIN);
  sha_ll_load(SHA2_256);
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
