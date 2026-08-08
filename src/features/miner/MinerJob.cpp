#include "config.h"
#if WITH_MINER

#include "MinerJob.h"
#include <string.h>

// ---------------------------------------------------------------------------
// hex
// ---------------------------------------------------------------------------
static inline uint8_t hexNibble(char ch) {
  if (ch >= '0' && ch <= '9') return (uint8_t)(ch - '0');
  if (ch >= 'a' && ch <= 'f') return (uint8_t)(ch - 'a' + 10);
  if (ch >= 'A' && ch <= 'F') return (uint8_t)(ch - 'A' + 10);
  return 0;
}

int minerHexToBytes(const char* in, size_t inLen, uint8_t* out) {
  int count = 0;
  for (size_t i = 0; i + 1 < inLen && in[i] && in[i + 1]; i += 2)
    out[count++] = (uint8_t)((hexNibble(in[i]) << 4) | hexNibble(in[i + 1]));
  return count;
}

// ---------------------------------------------------------------------------
// SHA-256 (FIPS 180-4), used for the coinbase and merkle folding — i.e. once
// per job, not in the hot loop. Kept here rather than calling mbedTLS so this
// file has no platform dependencies and so job preparation never contends with
// a mining worker for the ESP32's SHA peripheral.
// ---------------------------------------------------------------------------
static const uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define RR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static MINER_HOT void sha256Block(uint32_t* h, const uint8_t* p) {
  uint32_t w[64];
  for (int i = 0; i < 16; i++)
    w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
           ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = RR(w[i - 15], 7) ^ RR(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = RR(w[i - 2], 17) ^ RR(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
  uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
  for (int i = 0; i < 64; i++) {
    uint32_t S1 = RR(e, 6) ^ RR(e, 11) ^ RR(e, 25);
    uint32_t ch = (e & f) ^ ((~e) & g);
    uint32_t t1 = hh + S1 + ch + kK[i] + w[i];
    uint32_t S0 = RR(a, 2) ^ RR(a, 13) ^ RR(a, 22);
    uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    uint32_t t2 = S0 + maj;
    hh = g; g = f; f = e; e = d + t1;
    d = c; c = b; b = a; a = t1 + t2;
  }
  h[0] += a; h[1] += b; h[2] += c; h[3] += d;
  h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

MINER_HOT void minerSha256(const uint8_t* in, size_t len, uint8_t out[32]) {
  uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  size_t full = len / 64;
  for (size_t i = 0; i < full; i++) sha256Block(h, in + i * 64);

  uint8_t tail[128];
  size_t rem = len - full * 64;
  memcpy(tail, in + full * 64, rem);
  tail[rem++] = 0x80;
  size_t tailLen = (rem <= 56) ? 64 : 128;
  memset(tail + rem, 0, tailLen - rem);
  uint64_t bits = (uint64_t)len * 8;
  for (int i = 0; i < 8; i++) tail[tailLen - 1 - i] = (uint8_t)(bits >> (i * 8));
  sha256Block(h, tail);
  if (tailLen == 128) sha256Block(h, tail + 64);

  for (int i = 0; i < 8; i++) {
    out[i * 4]     = (uint8_t)(h[i] >> 24);
    out[i * 4 + 1] = (uint8_t)(h[i] >> 16);
    out[i * 4 + 2] = (uint8_t)(h[i] >> 8);
    out[i * 4 + 3] = (uint8_t)(h[i]);
  }
}

void minerSha256d(const uint8_t* in, size_t len, uint8_t out[32]) {
  uint8_t tmp[32];
  minerSha256(in, len, tmp);
  minerSha256(tmp, 32, out);
}

// The software mining path. The header's first block is identical for every
// nonce in a job, so it is compressed once into a midstate and only the second
// block is redone per attempt — the same trick the hardware engine cannot do,
// which is why software needs two compressions per nonce where hardware needs
// three.
//
// The unrolled predecessor was ~25 KB of IRAM; replacing it with this rolled
// loop was scored as costing ~7% of hashrate, which was wrong — that was its
// share of the total *after* the swap, not before. Measured on the device the
// software core went from ~110 KH/s to 16.3 KH/s, i.e. a quarter of the hybrid
// total, and nearly all of that was cache: the rolled loop is small but it was
// executing from flash on the core that also serves WiFi and the hardware SHA
// loop. Hence MINER_HOT below rather than a return to 25 KB of unrolling.
void minerMidstate(const uint8_t block1[64], uint32_t state[8]) {
  state[0] = 0x6a09e667; state[1] = 0xbb67ae85;
  state[2] = 0x3c6ef372; state[3] = 0xa54ff53a;
  state[4] = 0x510e527f; state[5] = 0x9b05688c;
  state[6] = 0x1f83d9ab; state[7] = 0x5be0cd19;
  sha256Block(state, block1);
}

MINER_HOT void minerSha256dFromMidstate(const uint32_t midstate[8],
                                       const uint8_t block2[64], uint8_t out[32]) {
  uint32_t h[8];
  memcpy(h, midstate, sizeof(h));
  sha256Block(h, block2);

  uint8_t first[32];
  for (int i = 0; i < 8; i++) {
    first[i * 4]     = (uint8_t)(h[i] >> 24);
    first[i * 4 + 1] = (uint8_t)(h[i] >> 16);
    first[i * 4 + 2] = (uint8_t)(h[i] >> 8);
    first[i * 4 + 3] = (uint8_t)(h[i]);
  }
  minerSha256(first, 32, out);
}

// ---------------------------------------------------------------------------
// target / difficulty
// ---------------------------------------------------------------------------
bool minerTargetFromNbits(const char* nbits, uint8_t targetLE[32]) {
  if (!nbits || strlen(nbits) != 8) return false;
  uint8_t b[4];
  if (minerHexToBytes(nbits, 8, b) != 4) return false;
  int exp = b[0];
  if (exp < 3 || exp > 32) return false;
  memset(targetLE, 0, 32);
  // value = mantissa * 256^(exp-3); in a little-endian array byte i has
  // weight 256^i, so the mantissa's low byte lands at index exp-3.
  targetLE[exp - 3] = b[3];
  targetLE[exp - 2] = b[2];
  targetLE[exp - 1] = b[1];
  return true;
}

bool minerHashMeetsTarget(const uint8_t* hash, const uint8_t targetLE[32]) {
  for (int i = 31; i >= 0; i--) {
    if (hash[i] < targetLE[i]) return true;
    if (hash[i] > targetLE[i]) return false;
  }
  return true;
}

static const double kTrueDiffOne =
    26959535291011309493156476344723991336010898738574164086137773096960.0;

double minerDiffFromHash(const uint8_t* hash) {
  double d = 0;
  for (int i = 3; i >= 0; i--) {           // little-endian 256-bit -> double
    uint64_t w = 0;
    for (int j = 7; j >= 0; j--) w = (w << 8) | hash[i * 8 + j];
    d = d * 18446744073709551616.0 + (double)w;
  }
  if (d <= 0) d = 1;
  return kTrueDiffOne / d;
}

// ---------------------------------------------------------------------------
// block header assembly
// ---------------------------------------------------------------------------

// Reverse each 4-byte word in place: stratum sends these fields big-endian for
// display, the block header carries them little-endian.
static void reverseWords(uint8_t* p, size_t len) {
  for (size_t i = 0; i < len; i += 4) {
    uint8_t t;
    t = p[i];     p[i]     = p[i + 3]; p[i + 3] = t;
    t = p[i + 1]; p[i + 1] = p[i + 2]; p[i + 2] = t;
  }
}

bool minerBuildWork(const char* version, const char* prevHash,
                    const char* coinb1, const char* extranonce1,
                    const char* extranonce2, const char* coinb2,
                    const uint8_t (*merkle)[32], uint8_t merkleCount,
                    const char* ntime, const char* nbits,
                    MinerWork& out) {
  if (!version || !prevHash || !coinb1 || !extranonce1 || !extranonce2 ||
      !coinb2 || !ntime || !nbits)
    return false;
  if (strlen(version) != 8 || strlen(prevHash) != 64 ||
      strlen(ntime) != 8 || strlen(nbits) != 8)
    return false;

  // coinbase = coinb1 || extranonce1 || extranonce2 || coinb2
  size_t hexLen = strlen(coinb1) + strlen(extranonce1) +
                  strlen(extranonce2) + strlen(coinb2);
  if ((hexLen & 1) || hexLen > MINER_MAX_COINBASE * 2) return false;

  static uint8_t coinbase[MINER_MAX_COINBASE];
  int n = 0;
  n += minerHexToBytes(coinb1, strlen(coinb1), coinbase + n);
  n += minerHexToBytes(extranonce1, strlen(extranonce1), coinbase + n);
  n += minerHexToBytes(extranonce2, strlen(extranonce2), coinbase + n);
  n += minerHexToBytes(coinb2, strlen(coinb2), coinbase + n);

  // merkle root: hash the coinbase, then fold in each branch left-to-right.
  uint8_t root[32];
  minerSha256d(coinbase, (size_t)n, root);
  for (uint8_t i = 0; i < merkleCount; i++) {
    uint8_t pair[64];
    memcpy(pair, root, 32);
    memcpy(pair + 32, merkle[i], 32);
    minerSha256d(pair, 64, root);
  }

  uint8_t* h = out.header;
  memset(h, 0, 128);
  if (minerHexToBytes(version, 8, h) != 4) return false;
  reverseWords(h, 4);
  if (minerHexToBytes(prevHash, 64, h + 4) != 32) return false;
  reverseWords(h + 4, 32);
  memcpy(h + 36, root, 32);                       // merkle root needs no swap
  if (minerHexToBytes(ntime, 8, h + 68) != 4) return false;
  reverseWords(h + 68, 4);
  if (minerHexToBytes(nbits, 8, h + 72) != 4) return false;
  reverseWords(h + 72, 4);
  // h[76..79] is the nonce, written per attempt by the hash workers.

  // SHA-256 padding for an 80-byte message: terminator then length 640 bits.
  h[80]  = 0x80;
  h[126] = 0x02;
  h[127] = 0x80;

  minerMidstate(h, out.midstate);

  return minerTargetFromNbits(nbits, out.targetLE);
}

#endif  // WITH_MINER
