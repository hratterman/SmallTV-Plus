// selftest.cpp — host-side checks for the miner's job math.
//
// The mining hot path is unobservable on the device: a wrong header byte or a
// mis-ported SHA round makes the cube hash happily forever and never find a
// share, which looks exactly like bad luck. So the platform-free half of the
// miner (MinerJob.cpp + NerdSha256.cpp) is compiled natively here and checked
// against known-good Bitcoin data.
//
// Build and run:  tools/miner_selftest/run.sh
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#include "MinerJob.h"
#include "NerdSha256.h"

static int g_fail = 0;

static void check(bool ok, const char* what) {
  printf("%s  %s\n", ok ? "  ok  " : "FAILED", what);
  if (!ok) g_fail++;
}

static void toHex(const uint8_t* b, size_t n, char* out) {
  for (size_t i = 0; i < n; i++) sprintf(out + i * 2, "%02x", b[i]);
}

// A block hash is displayed as the raw digest reversed.
static void hashToDisplay(const uint8_t* h, char* out) {
  uint8_t r[32];
  for (int i = 0; i < 32; i++) r[i] = h[31 - i];
  toHex(r, 32, out);
}

// ---------------------------------------------------------------------------
// 1. SHA-256 against the FIPS test vector.
// ---------------------------------------------------------------------------
static void testSha256() {
  uint8_t out[32];
  char hex[65];
  minerSha256((const uint8_t*)"abc", 3, out);
  toHex(out, 32, hex);
  check(!strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
        "SHA-256(\"abc\") matches the FIPS vector");

  // A message long enough to need a second padding block.
  const char* m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  minerSha256((const uint8_t*)m, strlen(m), out);
  toHex(out, 32, hex);
  check(!strcmp(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"),
        "SHA-256 of a 56-byte message (two-block padding path)");
}

// ---------------------------------------------------------------------------
// 2. The mining hot path against mainnet block 125552, whose header fields and
//    hash are a long-standing public test vector.
// ---------------------------------------------------------------------------
static void testBlock125552() {
  // Wire-order (little-endian field) header bytes, nonce zeroed.
  const char* headerHex =
      "01000000"
      "81cd02ab7e569e8bcd9317e2fe99f2de44d49ab2b8851ba4a308000000000000"
      "e320b6c2fffc8d750423db8b1eb942ae710e951ed797f7affc8892b0f1fc122b"
      "c7f5d74d"
      "f2b9441a"
      "00000000";
  const char* knownHash =
      "00000000000000001e8d6829a8a21adc5d38d0a473b144b6765798e61f98bd1d";
  const uint32_t winning = 0x9546a142;

  uint8_t header[128];
  memset(header, 0, sizeof(header));
  minerHexToBytes(headerHex, strlen(headerHex), header);
  header[80]  = 0x80;
  header[126] = 0x02;
  header[127] = 0x80;
  memcpy(header + 76, &winning, 4);

  char disp[65];

  // Pin the test vector itself first, with the generic SHA-256 verified above.
  // If this line fails the constants are mistranscribed, not the miner.
  uint8_t ref[32];
  minerSha256d(header, 80, ref);
  hashToDisplay(ref, disp);
  check(!strcmp(disp, knownHash),
        "test vector is sound: generic SHA-256d of the header gives the block hash");

  uint32_t midstate[8], bake[16];
  nerd_mids(midstate, header);
  nerd_sha256_bake(midstate, header + 64, bake);

  uint8_t hash[32];
  bool solved = nerd_sha256d_baked(midstate, header + 64, bake, hash);
  check(solved, "block 125552's nonce survives the 16-bit early exit");
  if (solved) {
    hashToDisplay(hash, disp);
    check(!strcmp(disp, knownHash),
          "nerd_sha256d_baked reproduces block 125552's hash");
    check(!memcmp(ref, hash, 32),
          "optimized and generic SHA-256d agree on the same header");
  }

  // The early exit must reject wrong nonces, and must never reject a good one.
  int survivors = 0;
  for (uint32_t n = winning - 500; n != winning + 500; n++) {
    memcpy(header + 76, &n, 4);
    uint8_t h2[32];
    if (nerd_sha256d_baked(midstate, header + 64, bake, h2)) {
      survivors++;
      // Anything that survives must genuinely end in 16 zero bits.
      check(h2[30] == 0 && h2[31] == 0, "a surviving hash really has 16 zero bits");
    }
  }
  check(survivors >= 1, "the early exit lets the winning nonce through");
  printf("       (%d of 1000 neighbouring nonces survived the early exit)\n", survivors);

  // Target and validity for this block.
  uint8_t targetLE[32];
  check(minerTargetFromNbits("1a44b9f2", targetLE), "nbits 1a44b9f2 expands");
  uint8_t targetBE[32];
  for (int i = 0; i < 32; i++) targetBE[i] = targetLE[31 - i];
  char thex[65];
  toHex(targetBE, 32, thex);
  check(!strcmp(thex, "00000000000044b9f20000000000000000000000000000000000000000000000"),
        "the expanded target matches block 125552's");
  check(minerHashMeetsTarget(hash, targetLE),
        "block 125552's hash meets its own network target");

  double diff = minerDiffFromHash(hash);
  printf("       (share difficulty of that hash: %.0f)\n", diff);
  check(diff > 1.0e9 && diff < 1.0e12, "share difficulty is in the expected range");
}

// ---------------------------------------------------------------------------
// 3. Full job assembly: coinbase -> merkle root -> header, against values
//    computed independently with Python's hashlib (see run.sh).
// ---------------------------------------------------------------------------
static void testJobAssembly(const char* expectedHeaderHex,
                            const char* expectedHashDisplay) {
  const char* version = "20000000";
  const char* prevHash = "000000000000000000024bead8df69990852c202db0e0097c1a12ea637d7e96d";
  const char* coinb1 = "01000000010000000000000000000000000000000000000000000000000000000000000000ffffffff20";
  const char* extranonce1 = "1a2b3c4d";
  const char* extranonce2 = "00000007";
  const char* coinb2 = "ffffffff0100f2052a010000001976a914000000000000000000000000000000000000000088ac00000000";
  const char* ntime = "5e0f1d20";
  const char* nbits = "170f48e4";

  uint8_t merkle[2][32];
  minerHexToBytes("57f0f8c8a1f3f9f0e7a54a5f5b1f0f3d2c9b8a7968574635241302f1e0d0c0b0", 64, merkle[0]);
  minerHexToBytes("0f1e2d3c4b5a69788796a5b4c3d2e1f00f1e2d3c4b5a69788796a5b4c3d2e1f0", 64, merkle[1]);

  MinerWork w;
  bool built = minerBuildWork(version, prevHash, coinb1, extranonce1, extranonce2,
                              coinb2, merkle, 2, ntime, nbits, w);
  check(built, "minerBuildWork accepts a well-formed job");
  if (!built) return;

  char hdr[161];
  toHex(w.header, 80, hdr);
  check(!strcmp(hdr, expectedHeaderHex),
        "assembled 80-byte header matches the reference implementation");
  if (strcmp(hdr, expectedHeaderHex)) {
    printf("         got %s\n         want %s\n", hdr, expectedHeaderHex);
  }

  // Hash it through the mining path and compare with the reference digest.
  const uint32_t nonce = 0x12345678;
  memcpy(w.header + 76, &nonce, 4);
  uint8_t ref[32], disp[65];
  minerSha256d(w.header, 80, ref);
  hashToDisplay(ref, (char*)disp);
  check(!strcmp((char*)disp, expectedHashDisplay),
        "header hashes to the reference digest for a fixed nonce");

  // And the optimized path must agree with the generic one, early exit aside.
  uint8_t fast[32];
  if (nerd_sha256d_baked(w.midstate, w.header + 64, w.bake, fast))
    check(!memcmp(fast, ref, 32), "optimized path agrees on the assembled header");
  else
    check(!(ref[30] == 0 && ref[31] == 0),
          "optimized path only early-exits on hashes that lack 16 zero bits");
}

// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: %s <expected-header-hex> <expected-hash-display>\n", argv[0]);
    return 2;
  }
  printf("miner self-test\n");
  printf("--- SHA-256 -------------------------------------------------\n");
  testSha256();
  printf("--- mining hot path (mainnet block 125552) ------------------\n");
  testBlock125552();
  printf("--- stratum job assembly ------------------------------------\n");
  testJobAssembly(argv[1], argv[2]);

  printf("-------------------------------------------------------------\n");
  printf(g_fail ? "%d CHECK(S) FAILED\n" : "all checks passed\n", g_fail);
  return g_fail ? 1 : 0;
}
