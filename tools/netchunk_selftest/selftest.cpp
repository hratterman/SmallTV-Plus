// Host checks for the streaming chunked-transfer decoder (src/NetChunk.h):
// the framing bug class that turned a good Open-Meteo reply into
// "unexpected reply" must never come back quietly.
#include <cstdio>
#include <cstring>
#include <string>

#include "../../src/NetChunk.h"

static int failures = 0;
static void ck(bool cond, const char* what) {
  printf("  %-5s %s\n", cond ? "ok" : "FAIL", what);
  if (!cond) failures++;
}

static bool emitStr(void* ctx, const uint8_t* p, uint16_t n) {
  ((std::string*)ctx)->append((const char*)p, n);
  return true;
}

// Run the same wire bytes through with a given feed window size.
static std::string decode(const char* wire, int window) {
  NetChunkDec d;
  std::string out;
  const int len = (int)strlen(wire);
  for (int i = 0; i < len; i += window) {
    int n = len - i < window ? len - i : window;
    if (!netChunkFeed(d, (const uint8_t*)wire + i, n,
                      emitStr, &out))
      break;
  }
  return out;
}

int main() {
  // {"lat":39 is 9 bytes, ,"t":72.8} is 10 (0xA).
  const char* wire2 =
      "9\r\n{\"lat\":39\r\n"
      "A\r\n,\"t\":72.8}\r\n"
      "0\r\n\r\n";
  const char* want = "{\"lat\":39,\"t\":72.8}";

  printf("--- reassembly at every feed granularity ---------------------\n");
  ck(decode(wire2, 1024) == want, "whole response in one feed");
  ck(decode(wire2, 1) == want, "one byte at a time");
  ck(decode(wire2, 3) == want, "three bytes at a time");
  ck(decode(wire2, 7) == want, "seven bytes at a time");

  printf("\n--- framing details ------------------------------------------\n");
  ck(decode("a;ext=1\r\n0123456789\r\n0\r\n\r\n", 4) == "0123456789",
     "chunk extensions are skipped");
  ck(decode("A\r\n0123456789\r\n0\r\n\r\ntrailer: x\r\n\r\n", 5) == "0123456789",
     "trailers after the last chunk are ignored");
  ck(decode("14\r\n01234567890123456789\r\n0\r\n\r\n", 6) == "01234567890123456789",
     "multi-digit hex sizes decode (0x14 = 20)");
  {
    // The terminal chunk must stop the feed loop (returns false).
    NetChunkDec d;
    std::string out;
    const char* w = "1\r\nx\r\n0\r\n\r\n";
    bool more = true;
    for (int i = 0; w[i] && more; i++)
      more = netChunkFeed(d, (const uint8_t*)w + i, 1, emitStr, &out);
    ck(!more && out == "x", "the 0 chunk ends the stream");
  }

  printf("\n-------------------------------------------------------------\n");
  if (failures) { printf("%d check(s) FAILED\n", failures); return 1; }
  printf("all checks passed\n");
  return 0;
}
