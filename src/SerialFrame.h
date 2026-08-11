// SerialFrame.h — packet framing for the USB tether, on a line that is also
// carrying a debug log.
//
// The cube has one UART and it is already full of Serial.println. Rather than
// silence the log — which is the thing you need most when a tether misbehaves —
// frames are made recognisable and the host treats everything else as text.
//
// SLIP framing does that job with no ambiguity: a frame is delimited by 0xC0,
// and any 0xC0 or 0xDB inside it is escaped, so a frame can carry arbitrary
// binary (a JPEG, say) without ever producing a stray delimiter. Log lines are
// ASCII and contain neither byte, so they fall between frames and the host can
// simply print them.
//
// A CRC over the header and payload is what makes the split reliable rather
// than merely likely: a log line that happened to contain 0xC0 would produce a
// candidate frame that fails the check and is discarded as text.
//
// Pure encode/decode with no I/O, so tools/serialframe_selftest can round-trip
// it on a host.
#pragma once
#include <stdint.h>
#include <string.h>

#define SF_END      0xC0
#define SF_ESC      0xDB
#define SF_ESC_END  0xDC
#define SF_ESC_ESC  0xDD

#define SF_VERSION  1
#define SF_HEADER   6      // ver, type, id(2), len(2)
#define SF_CRC      2
// One frame. Sized by the largest thing that has to arrive whole: a Spotify
// request carries a ~300-character bearer token in its headers on top of the
// URL. Response bodies are not bound by this — they stream as many frames.
#define SF_MAX_PAYLOAD 1024

// Frame types. Device->host are requests, host->device are answers.
enum SerialFrameType : uint8_t {
  SF_HELLO       = 0x01,   // device: I am here, this is my firmware
  SF_HELLO_ACK   = 0x02,   // host: tether is up
  SF_HTTP_REQ    = 0x10,   // device: perform this request
  SF_HTTP_STATUS = 0x11,   // host: status code + content length
  SF_HTTP_DATA   = 0x12,   // host: a chunk of body
  SF_HTTP_END    = 0x13,   // host: body complete
  SF_HTTP_ERR    = 0x14,   // host: could not perform it, reason in payload
  SF_TIME        = 0x20,   // host: unix epoch, so a tethered cube needs no NTP
  SF_LOG         = 0x30,   // device: a log line, framed rather than loose

  // Settings over the same cable. The page that lends the cube its internet is
  // also the only thing that can reach it on a network it could not join, so it
  // doubles as the configuration UI. Both directions chunk, because the config
  // document is several KB and a frame holds one.
  SF_CFG_GET     = 0x40,   // host: send me the settings
  SF_CFG_DATA    = 0x41,   // device: a slice of the settings JSON
  SF_CFG_END     = 0x42,   // device: that was all of it
  SF_CFG_SET     = 0x43,   // host: a slice of new settings
  SF_CFG_APPLY   = 0x44,   // host: that was all of it, apply and save
  SF_CFG_OK      = 0x45,   // device: applied (payload: "ok" or the reason not)
  SF_ICS_DATA    = 0x46,   // host: a slice of a calendar (.ics) file
  SF_ICS_END     = 0x47,   // host: that was all of it; device replies with the same
                           // type carrying "ok: N events" or the reason not

  // Firmware over the same cable: a tethered cube may never join WiFi, and
  // without this it could never be updated again. Same Update machinery and
  // wrong-file guard as the web upload; the page paces itself on the per-chunk
  // acks (empty reply = written, text = the reason it stopped).
  SF_OTA_BEGIN   = 0x48,   // host: 4-byte LE image size; device replies "ok"/reason
  SF_OTA_DATA    = 0x49,   // host: a slice; device acks with empty same-type frame
  SF_OTA_END     = 0x4A,   // host: done; device replies "ok" then reboots, or the reason
  SF_STAT_GET    = 0x4B,   // host: send a small status JSON; device replies same type
};

// CRC-16/CCITT-FALSE. Chosen for being three lines rather than for pedigree —
// this is guarding against a log line that looks like a frame, not against an
// adversary.
static inline uint16_t sfCrc16(const uint8_t* p, uint32_t n, uint16_t crc = 0xFFFF) {
  while (n--) {
    crc ^= (uint16_t)(*p++) << 8;
    for (uint8_t i = 0; i < 8; i++)
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
  }
  return crc;
}

// Worst case on the wire: every byte escaped, plus both delimiters.
static inline uint32_t sfEncodedMax(uint32_t payloadLen) {
  return 2 + 2 * (SF_HEADER + payloadLen + SF_CRC);
}

// Encode one frame into `out`. Returns bytes written, or 0 if it would not fit
// or the payload is over the cap.
static inline uint32_t sfEncode(uint8_t type, uint16_t id,
                                const uint8_t* payload, uint16_t len,
                                uint8_t* out, uint32_t outCap) {
  if (len > SF_MAX_PAYLOAD) return 0;
  if (outCap < sfEncodedMax(len)) return 0;

  uint8_t hdr[SF_HEADER];
  hdr[0] = SF_VERSION;
  hdr[1] = type;
  hdr[2] = (uint8_t)(id & 0xFF);
  hdr[3] = (uint8_t)(id >> 8);
  hdr[4] = (uint8_t)(len & 0xFF);
  hdr[5] = (uint8_t)(len >> 8);

  uint16_t crc = sfCrc16(hdr, SF_HEADER);
  if (len && payload) crc = sfCrc16(payload, len, crc);

  uint32_t n = 0;
  out[n++] = SF_END;

  // One escaper for the header, the payload and the CRC alike.
  struct Esc {
    static void put(uint8_t* o, uint32_t& n, uint8_t b) {
      if (b == SF_END)      { o[n++] = SF_ESC; o[n++] = SF_ESC_END; }
      else if (b == SF_ESC) { o[n++] = SF_ESC; o[n++] = SF_ESC_ESC; }
      else                  { o[n++] = b; }
    }
  };
  for (uint32_t i = 0; i < SF_HEADER; i++) Esc::put(out, n, hdr[i]);
  for (uint32_t i = 0; i < len; i++)       Esc::put(out, n, payload[i]);
  Esc::put(out, n, (uint8_t)(crc >> 8));
  Esc::put(out, n, (uint8_t)(crc & 0xFF));

  out[n++] = SF_END;
  return n;
}

// Incremental decoder. Feed it bytes as they arrive; it calls nothing and
// allocates nothing, it just tells you when a complete frame is sitting in its
// buffer. Anything that is not a valid frame is dropped, which is exactly what
// should happen to a log line.
struct SerialFrameDecoder {
  uint8_t  buf[SF_HEADER + SF_MAX_PAYLOAD + SF_CRC];
  uint32_t len = 0;
  bool     inFrame = false;
  bool     escaped = false;
  bool     overrun = false;   // a frame longer than the buffer; drop to the next END

  // Fields of the last frame accepted by feed().
  uint8_t  type = 0;
  uint16_t id = 0;
  uint16_t payloadLen = 0;
  const uint8_t* payload = nullptr;

  void reset() { len = 0; escaped = false; overrun = false; }

  // Returns true when `b` completed a valid frame.
  bool feed(uint8_t b) {
    if (b == SF_END) {
      const bool done = inFrame && !overrun && finish();
      inFrame = true;          // an END both closes one frame and opens the next
      reset();
      return done;
    }
    if (!inFrame) return false;

    if (escaped) {
      escaped = false;
      if (b == SF_ESC_END)      b = SF_END;
      else if (b == SF_ESC_ESC) b = SF_ESC;
      else { overrun = true; return false; }   // invalid escape: not a frame
    } else if (b == SF_ESC) {
      escaped = true;
      return false;
    }

    if (len >= sizeof(buf)) { overrun = true; return false; }
    buf[len++] = b;
    return false;
  }

 private:
  bool finish() {
    if (len < SF_HEADER + SF_CRC) return false;
    if (buf[0] != SF_VERSION) return false;
    const uint16_t declared = (uint16_t)buf[4] | ((uint16_t)buf[5] << 8);
    if (len != (uint32_t)SF_HEADER + declared + SF_CRC) return false;

    const uint16_t want = ((uint16_t)buf[len - 2] << 8) | buf[len - 1];
    if (sfCrc16(buf, SF_HEADER + declared) != want) return false;

    type = buf[1];
    id = (uint16_t)buf[2] | ((uint16_t)buf[3] << 8);
    payloadLen = declared;
    payload = declared ? (buf + SF_HEADER) : nullptr;
    return true;
  }
};
