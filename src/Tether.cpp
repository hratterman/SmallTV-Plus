#include "config.h"
#if WITH_TETHER

#include "Tether.h"
#include "SerialFrame.h"
#include <time.h>
#include <sys/time.h>

#if defined(ESP32) || defined(ARDUINO_ARCH_ESP32)
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
static SemaphoreHandle_t s_lock = nullptr;
static inline void linkTake() { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
static inline void linkGive() { if (s_lock) xSemaphoreGive(s_lock); }
static inline bool linkTry(uint32_t ms) {
  return !s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(ms)) == pdTRUE;
}
#else
static inline void linkTake() {}
static inline void linkGive() {}
static inline bool linkTry(uint32_t) { return true; }
#endif

static SerialFrameDecoder s_dec;
static uint32_t s_lastHeard = 0;     // millis() of the last frame from the host
static uint32_t s_lastHello = 0;
static bool     s_everSeen = false;
static uint16_t s_nextId = 1;

// Scratch for one outgoing frame. Static rather than stack: the request path is
// called from the render task, whose stack is not generous.
static uint8_t s_wire[SF_HEADER + SF_MAX_PAYLOAD + SF_CRC + 64];
static uint8_t s_req[SF_MAX_PAYLOAD];

// Supplied by main.cpp. Kept as callbacks so this file stays a transport and
// does not grow a dependency on the settings layer.
static void (*s_cfgSerialise)(String&) = nullptr;
static const char* (*s_cfgApply)(const String&) = nullptr;
static void        (*s_icsFeed)(const uint8_t*, uint16_t) = nullptr;
static const char* (*s_icsDone)() = nullptr;

void tetherOnIcs(void (*feed)(const uint8_t*, uint16_t), const char* (*done)()) {
  s_icsFeed = feed;
  s_icsDone = done;
}
static String s_cfgIn;          // accumulates SF_CFG_SET chunks

void tetherOnConfig(void (*serialise)(String& out),
                    const char* (*apply)(const String& json)) {
  s_cfgSerialise = serialise;
  s_cfgApply = apply;
}

void tetherBegin() {
#if defined(ESP32) || defined(ARDUINO_ARCH_ESP32)
  if (!s_lock) s_lock = xSemaphoreCreateMutex();
#endif
  s_lastHeard = 0;
  s_everSeen = false;
}

bool tetherActive() {
  return s_everSeen && (millis() - s_lastHeard) < TETHER_IDLE_MS;
}

uint32_t tetherIdleSec() {
  return s_everSeen ? (millis() - s_lastHeard) / 1000 : 0;
}

static void writeFrame(uint8_t type, uint16_t id, const uint8_t* p, uint16_t n) {
  const uint32_t len = sfEncode(type, id, p, n, s_wire, sizeof(s_wire));
  if (!len) return;
  // One write call for the whole frame: the shorter the window, the smaller the
  // chance another task's log line lands inside it.
  Serial.write(s_wire, len);
}

// The host's clock, taken as offered. A tethered cube has no reason to ask the
// network for the time and often no route to do it — this arrives with the
// first handshake, before anything needs it.
static void applyTime(const uint8_t* p, uint16_t n) {
  if (n < 8) return;
  int64_t epoch = 0;
  for (int i = 7; i >= 0; i--) epoch = (epoch << 8) | p[i];
  if (epoch < 1700000000LL) return;         // obviously wrong; ignore it
  struct timeval tv = {(time_t)epoch, 0};
  settimeofday(&tv, nullptr);
}

// Send the settings as a run of chunks. Serialised into a String first: the
// document is a few KB and building it twice to measure it would cost more than
// holding it once.
static void sendConfig() {
  if (!s_cfgSerialise) return;
  String json;
  s_cfgSerialise(json);
  const uint32_t n = json.length();
  for (uint32_t o = 0; o < n; o += TETHER_CFG_CHUNK) {
    const uint16_t len = (uint16_t)((n - o) < TETHER_CFG_CHUNK ? (n - o) : TETHER_CFG_CHUNK);
    writeFrame(SF_CFG_DATA, 0, (const uint8_t*)json.c_str() + o, len);
  }
  writeFrame(SF_CFG_END, 0, nullptr, 0);
  Serial.printf("[tether] sent %u bytes of settings\n", (unsigned)n);
}

// Handle one frame that is not part of a reply. Returns true if it was
// consumed here.
static bool handleAside(uint8_t type, const uint8_t* p, uint16_t n) {
  switch (type) {
    case SF_CFG_GET:
      s_lastHeard = millis();
      sendConfig();
      return true;
    case SF_CFG_SET:
      s_lastHeard = millis();
      // Bounded: a settings document that will not fit is a bug or an attack,
      // and either way should not be allowed to exhaust the heap.
      if (s_cfgIn.length() + n <= TETHER_CFG_MAX) s_cfgIn.concat((const char*)p, n);
      return true;
    case SF_CFG_APPLY: {
      s_lastHeard = millis();
      const char* err = s_cfgApply ? s_cfgApply(s_cfgIn) : "no handler";
      s_cfgIn = "";
      const char* reply = err ? err : "ok";
      writeFrame(SF_CFG_OK, 0, (const uint8_t*)reply, (uint16_t)strlen(reply));
      Serial.printf("[tether] settings %s\n", err ? err : "applied");
      return true;
    }
    case SF_ICS_DATA:
      s_lastHeard = millis();
      if (s_icsFeed) s_icsFeed(p, n);
      return true;
    case SF_ICS_END: {
      s_lastHeard = millis();
      const char* r = s_icsDone ? s_icsDone() : "calendar not compiled in";
      writeFrame(SF_ICS_END, 0, (const uint8_t*)r, (uint16_t)strlen(r));
      Serial.printf("[tether] calendar import: %s\n", r);
      return true;
    }
    case SF_HELLO_ACK:
      if (!s_everSeen) Serial.println("[tether] host answered; using the cable");
      s_everSeen = true;
      s_lastHeard = millis();
      return true;
    case SF_TIME:
      applyTime(p, n);
      s_lastHeard = millis();
      return true;
    default:
      return false;
  }
}

void tetherService() {
  const uint32_t now = millis();

  // Announce ourselves until a host answers, then keep a slow heartbeat so
  // unplugging the cable is noticed rather than assumed.
  const uint32_t helloEvery = tetherActive() ? TETHER_HELLO_UP_MS : TETHER_HELLO_DOWN_MS;
  if (now - s_lastHello >= helloEvery) {
    s_lastHello = now;
    if (linkTry(5)) {
      static const char kWho[] = FW_NAME " " FW_VERSION;
      writeFrame(SF_HELLO, 0, (const uint8_t*)kWho, sizeof(kWho) - 1);
      linkGive();
    }
  }

  if (!linkTry(2)) return;      // a fetch owns the link; it is doing the reading
  int budget = 512;             // bounded so the loop never stalls on a busy line
  while (budget-- > 0 && Serial.available()) {
    if (s_dec.feed((uint8_t)Serial.read()))
      handleAside(s_dec.type, s_dec.payload, s_dec.payloadLen);
  }
  linkGive();

  if (s_everSeen && !tetherActive() && (now - s_lastHeard) < TETHER_IDLE_MS + 2000) {
    Serial.println("[tether] host went quiet; back to WiFi");
  }
}

// ---------------------------------------------------------------------------
static uint16_t packRequest(const char* url, bool post, const char* headers,
                            const uint8_t* body, uint16_t bodyLen) {
  const uint16_t urlLen = (uint16_t)strlen(url);
  const uint16_t hdrLen = headers ? (uint16_t)strlen(headers) : 0;
  const uint32_t need = 1 + 2 + urlLen + 2 + hdrLen + 2 + bodyLen;
  if (need > sizeof(s_req)) return 0;

  uint16_t o = 0;
  s_req[o++] = post ? 1 : 0;
  s_req[o++] = (uint8_t)(urlLen & 0xFF); s_req[o++] = (uint8_t)(urlLen >> 8);
  memcpy(s_req + o, url, urlLen); o += urlLen;
  s_req[o++] = (uint8_t)(hdrLen & 0xFF); s_req[o++] = (uint8_t)(hdrLen >> 8);
  if (hdrLen) { memcpy(s_req + o, headers, hdrLen); o += hdrLen; }
  s_req[o++] = (uint8_t)(bodyLen & 0xFF); s_req[o++] = (uint8_t)(bodyLen >> 8);
  if (bodyLen && body) { memcpy(s_req + o, body, bodyLen); o += bodyLen; }
  return o;
}

TetherResult tetherFetch(const char* url, bool post, const char* headers,
                         const uint8_t* body, uint16_t bodyLen,
                         TetherSink sink, void* ctx, uint32_t timeoutMs) {
  TetherResult r = {};
  r.error[0] = 0;

  if (!tetherActive()) { strlcpy(r.error, "no tether", sizeof(r.error)); return r; }

  const uint16_t n = packRequest(url, post, headers, body, bodyLen);
  if (!n) { strlcpy(r.error, "request too large for one frame", sizeof(r.error)); return r; }

  linkTake();
  const uint16_t id = s_nextId++ ? s_nextId : (s_nextId = 1);
  writeFrame(SF_HTTP_REQ, id, s_req, n);

  const uint32_t deadline = millis() + timeoutMs;
  bool gotStatus = false, done = false, abandoned = false;

  while (!done && (int32_t)(millis() - deadline) < 0) {
    if (!Serial.available()) { delay(2); continue; }
    if (!s_dec.feed((uint8_t)Serial.read())) continue;

    // Anything for a previous request is stale by definition — one at a time.
    if (s_dec.id != id) { handleAside(s_dec.type, s_dec.payload, s_dec.payloadLen); continue; }
    s_lastHeard = millis();

    switch (s_dec.type) {
      case SF_HTTP_STATUS:
        if (s_dec.payloadLen >= 2) {
          r.status = (int)((uint16_t)s_dec.payload[0] | ((uint16_t)s_dec.payload[1] << 8));
          gotStatus = true;
        }
        break;
      case SF_HTTP_DATA:
        if (!abandoned && sink && s_dec.payloadLen) {
          if (!sink(ctx, s_dec.payload, s_dec.payloadLen)) abandoned = true;
        }
        r.bytes += s_dec.payloadLen;
        break;
      case SF_HTTP_END:
        r.ok = gotStatus;
        done = true;
        break;
      case SF_HTTP_ERR: {
        const uint16_t m = s_dec.payloadLen < sizeof(r.error) - 1
                             ? s_dec.payloadLen : (uint16_t)(sizeof(r.error) - 1);
        memcpy(r.error, s_dec.payload, m);
        r.error[m] = 0;
        done = true;
        break;
      }
      default:
        handleAside(s_dec.type, s_dec.payload, s_dec.payloadLen);
        break;
    }
  }
  linkGive();

  if (!done && !r.error[0]) strlcpy(r.error, "tether timed out", sizeof(r.error));
  return r;
}

#endif  // WITH_TETHER
