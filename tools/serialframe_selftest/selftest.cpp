// Host-side checks for src/SerialFrame.h.
//
// The properties that matter for a tether sharing its UART with a debug log:
// a frame survives a round trip byte-for-byte including bytes that collide
// with the delimiters, and log text mixed into the stream is discarded rather
// than mistaken for a frame.
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include "../../src/SerialFrame.h"

static int failures = 0;
static void ck(bool cond, const char* what) {
  printf("  %-5s %s\n", cond ? "ok" : "FAIL", what);
  if (!cond) failures++;
}

// Push bytes through a decoder, returning how many frames came out and keeping
// the last one.
static int pump(SerialFrameDecoder& d, const uint8_t* p, uint32_t n,
                uint8_t* lastPayload, uint16_t* lastLen, uint8_t* lastType) {
  int frames = 0;
  for (uint32_t i = 0; i < n; i++) {
    if (d.feed(p[i])) {
      frames++;
      if (lastLen) *lastLen = d.payloadLen;
      if (lastType) *lastType = d.type;
      if (lastPayload && d.payloadLen) memcpy(lastPayload, d.payload, d.payloadLen);
    }
  }
  return frames;
}

int main() {
  static uint8_t wire[8192], got[SF_MAX_PAYLOAD];
  uint16_t gotLen = 0;
  uint8_t  gotType = 0;

  printf("--- round trip ----------------------------------------------\n");
  {
    const char* msg = "GET https://api.spotify.com/v1/me/player/currently-playing";
    uint32_t n = sfEncode(SF_HTTP_REQ, 0x1234, (const uint8_t*)msg,
                          (uint16_t)strlen(msg), wire, sizeof(wire));
    ck(n > 0, "encodes");
    SerialFrameDecoder d;
    int frames = pump(d, wire, n, got, &gotLen, &gotType);
    ck(frames == 1, "one frame out");
    ck(gotType == SF_HTTP_REQ, "type survives");
    ck(d.id == 0x1234, "id survives");
    ck(gotLen == strlen(msg) && memcmp(got, msg, gotLen) == 0, "payload survives");
  }

  printf("\n--- binary payloads with delimiter bytes in them ------------\n");
  {
    // A JPEG will contain 0xC0 and 0xDB constantly — 0xDB is literally the JPEG
    // quantisation-table marker byte.
    uint8_t blob[300];
    for (int i = 0; i < 300; i++) blob[i] = (uint8_t)(i % 256);
    uint32_t n = sfEncode(SF_HTTP_DATA, 7, blob, sizeof(blob), wire, sizeof(wire));
    ck(n > 0, "encodes a payload containing 0xC0 and 0xDB");
    SerialFrameDecoder d;
    int frames = pump(d, wire, n, got, &gotLen, &gotType);
    ck(frames == 1 && gotLen == sizeof(blob), "one frame, right length");
    ck(memcmp(got, blob, sizeof(blob)) == 0, "every byte identical after escaping");
  }
  {
    uint8_t worst[64];
    memset(worst, SF_END, sizeof(worst));
    uint32_t n = sfEncode(SF_HTTP_DATA, 1, worst, sizeof(worst), wire, sizeof(wire));
    ck(n > 0 && n <= sfEncodedMax(sizeof(worst)), "all-delimiter payload fits the stated bound");
    SerialFrameDecoder d;
    ck(pump(d, wire, n, got, &gotLen, nullptr) == 1 && gotLen == sizeof(worst),
       "and round-trips");
  }

  printf("\n--- log text on the same line -------------------------------\n");
  {
    const char* log = "[miner] worker 0 on core 0\n[boot] done\n";
    SerialFrameDecoder d;
    ck(pump(d, (const uint8_t*)log, strlen(log), nullptr, nullptr, nullptr) == 0,
       "plain log text yields no frames");
  }
  {
    // Log text either side of a real frame: the frame must still arrive.
    const char* pre = "[captive] portal: 302 redirect\n";
    const char* post = "[net] rssi -54\n";
    const char* body = "hello";
    uint32_t n = sfEncode(SF_HELLO, 9, (const uint8_t*)body, 5, wire, sizeof(wire));
    static uint8_t mixed[8192];
    uint32_t m = 0;
    memcpy(mixed + m, pre, strlen(pre));   m += strlen(pre);
    memcpy(mixed + m, wire, n);            m += n;
    memcpy(mixed + m, post, strlen(post)); m += strlen(post);
    SerialFrameDecoder d;
    ck(pump(d, mixed, m, got, &gotLen, &gotType) == 1, "frame found between log lines");
    ck(gotType == SF_HELLO && gotLen == 5 && memcmp(got, body, 5) == 0,
       "and is intact");
  }
  {
    // A log line that happens to contain the delimiter must not be accepted.
    uint8_t evil[64];
    uint32_t m = 0;
    evil[m++] = SF_END;
    const char* junk = "not a frame at all";
    memcpy(evil + m, junk, strlen(junk)); m += strlen(junk);
    evil[m++] = SF_END;
    SerialFrameDecoder d;
    ck(pump(d, evil, m, nullptr, nullptr, nullptr) == 0,
       "delimiter-wrapped junk fails the CRC and is dropped");
  }

  printf("\n--- corruption ----------------------------------------------\n");
  {
    const char* body = "abcdefgh";
    uint32_t n = sfEncode(SF_HTTP_DATA, 3, (const uint8_t*)body, 8, wire, sizeof(wire));
    wire[5] ^= 0x01;                       // flip a bit in the payload
    SerialFrameDecoder d;
    ck(pump(d, wire, n, nullptr, nullptr, nullptr) == 0, "a flipped bit is rejected");
  }
  {
    // Truncated frame followed by a good one: the decoder must recover.
    const char* body = "recovered";
    uint32_t n = sfEncode(SF_HELLO, 4, (const uint8_t*)body, 9, wire, sizeof(wire));
    static uint8_t mixed[8192];
    uint32_t m = 0;
    memcpy(mixed, wire, n / 2); m = n / 2;          // half a frame
    memcpy(mixed + m, wire, n); m += n;             // then a whole one
    SerialFrameDecoder d;
    ck(pump(d, mixed, m, got, &gotLen, nullptr) == 1, "recovers after a truncated frame");
    ck(gotLen == 9 && memcmp(got, body, 9) == 0, "and the good frame is intact");
  }

  printf("\n--- limits ---------------------------------------------------\n");
  {
    static uint8_t big[SF_MAX_PAYLOAD + 1];
    memset(big, 'x', sizeof(big));
    ck(sfEncode(SF_HTTP_DATA, 1, big, SF_MAX_PAYLOAD + 1, wire, sizeof(wire)) == 0,
       "an over-long payload is refused rather than truncated");
    uint32_t n = sfEncode(SF_HTTP_DATA, 1, big, SF_MAX_PAYLOAD, wire, sizeof(wire));
    ck(n > 0, "the maximum payload encodes");
    SerialFrameDecoder d;
    ck(pump(d, wire, n, got, &gotLen, nullptr) == 1 && gotLen == SF_MAX_PAYLOAD,
       "and decodes");
  }
  {
    uint8_t tiny[8];
    ck(sfEncode(SF_HELLO, 1, nullptr, 0, tiny, sizeof(tiny)) == 0,
       "refuses to encode into a buffer that cannot hold the result");
  }

  printf("\n--- back to back ---------------------------------------------\n");
  {
    // Frames sharing a delimiter, as they will on a busy line.
    uint32_t m = 0;
    for (int i = 0; i < 5; i++) {
      char b[16];
      snprintf(b, sizeof(b), "frame%d", i);
      m += sfEncode(SF_HTTP_DATA, (uint16_t)i, (const uint8_t*)b,
                    (uint16_t)strlen(b), wire + m, sizeof(wire) - m);
    }
    SerialFrameDecoder d;
    ck(pump(d, wire, m, got, &gotLen, nullptr) == 5, "five frames in, five out");
  }

  printf("\n-------------------------------------------------------------\n");
  if (failures) { printf("%d check(s) FAILED\n", failures); return 1; }
  printf("all checks passed\n");
  return 0;
}
