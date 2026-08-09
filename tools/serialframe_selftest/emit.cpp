// Writes a fixed set of frames, encoded by the firmware's own SerialFrame.h, to
// stdout. tools/tether.py decodes them, which is what stops the two
// implementations quietly disagreeing about endianness or the CRC.
#include <cstdio>
#include <cstring>
#include "../../src/SerialFrame.h"

int main() {
  static uint8_t wire[8192];
  uint32_t n;

  const char* hello = "smalltv-mod";
  n = sfEncode(SF_HELLO, 1, (const uint8_t*)hello, (uint16_t)strlen(hello),
               wire, sizeof(wire));
  fwrite(wire, 1, n, stdout);

  uint8_t ramp[256];
  for (int i = 0; i < 256; i++) ramp[i] = (uint8_t)i;
  n = sfEncode(SF_HTTP_DATA, 0x1234, ramp, sizeof(ramp), wire, sizeof(wire));
  fwrite(wire, 1, n, stdout);

  n = sfEncode(SF_HTTP_END, 0xBEEF, nullptr, 0, wire, sizeof(wire));
  fwrite(wire, 1, n, stdout);
  return 0;
}
