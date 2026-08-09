// Reads a frame stream on stdin with the firmware's decoder and prints
// "type id payloadLen" per frame. The other half of the cross-check: bytes
// produced by tools/tether.py have to parse here.
#include <cstdio>
#include "../../src/SerialFrame.h"

int main() {
  SerialFrameDecoder d;
  int c;
  while ((c = fgetc(stdin)) != EOF) {
    if (d.feed((uint8_t)c))
      printf("%u %u %u\n", (unsigned)d.type, (unsigned)d.id, (unsigned)d.payloadLen);
  }
  return 0;
}
