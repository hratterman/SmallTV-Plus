// NetChunk.h — a streaming HTTP/1.1 chunked-transfer decoder.
//
// Chunked responses interleave hex size lines with the payload. Neither
// Arduino core's raw stream undoes that framing, so a JSON body parsed
// straight off the wire fails on the first "149\r\n" — which is exactly how
// the weather fetch turned a perfectly good Open-Meteo reply into
// "unexpected reply". The decoder is fed whatever byte windows the socket
// happens to deliver and emits only payload bytes; it is a plain header so
// the host self-test can drip bytes through it one at a time.
#pragma once
#include <stdint.h>

struct NetChunkDec {
  uint8_t state = 0;   // 0 size line, 1 data, 2 skip-to-LF after data,
                       // 3 skip-to-LF after a non-hex (chunk extension), 4 done
  long    left  = 0;   // payload bytes remaining in the current chunk
};

// Feed `n` raw bytes; `emit(ctx, ptr, len)` receives payload bytes only.
// Returns false once the terminal 0-length chunk has been consumed (trailers
// are ignored) — the caller can stop reading the socket then.
template <typename EmitFn>
static inline bool netChunkFeed(NetChunkDec& d, const uint8_t* buf, int n,
                                EmitFn emit, void* ctx) {
  int i = 0;
  while (i < n) {
    const uint8_t c = buf[i];
    switch (d.state) {
      case 0: {                                  // the hex size line
        if (c == '\n') {
          d.state = d.left ? 1 : 4;
          i++;
        } else if (c == '\r') {
          i++;
        } else {
          const int v = (c >= '0' && c <= '9')   ? c - '0'
                        : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                        : (c >= 'A' && c <= 'F') ? c - 'A' + 10
                                                 : -1;
          if (v >= 0) {
            d.left = d.left * 16 + v;
            i++;
          } else {
            d.state = 3;                         // ";ext=..." — skip to LF
          }
        }
        break;
      }
      case 1: {                                  // payload
        int take = n - i;
        if ((long)take > d.left) take = (int)d.left;
        if (take > 0) emit(ctx, buf + i, (uint16_t)take);
        i += take;
        d.left -= take;
        if (d.left == 0) d.state = 2;
        break;
      }
      case 2:                                    // CRLF after the payload
        i++;
        if (c == '\n') d.state = 0;
        break;
      case 3:                                    // rest of the size line
        i++;
        if (c == '\n') d.state = d.left ? 1 : 4;
        break;
      default:                                   // 4: done — drain and ignore
        return false;
    }
    if (d.state == 4) return false;
  }
  return d.state != 4;
}
