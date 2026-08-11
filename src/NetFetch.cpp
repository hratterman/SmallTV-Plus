#include "NetChunk.h"
#include "Platform.h"
#include "NetFetch.h"
#include "Net.h"

#if WITH_TETHER
#include "Tether.h"
#endif

#if defined(SMALLTV_ESP8266)
#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <ESP8266HTTPClient.h>
#else
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#endif

bool netHaveRoute() { return netFetchTethered() || netConnected(); }

bool netFetchTethered() {
#if WITH_TETHER
  return tetherActive();
#else
  return false;
#endif
}

// ---------------------------------------------------------------------------
// Resolve a URL's host separately before any HTTP client touches it: the
// clients fold "DNS said no", "DNS answered 0.0.0.0" (a filtering resolver's
// block answer - hotspots and hotel WiFi do this) and "TCP refused" into one
// opaque failure. On a filtered network the difference IS the diagnosis.
bool netDnsPrecheck(const char* url, char* err, size_t errLen, char* ipOut) {
  if (ipOut) ipOut[0] = 0;
  char host[64];
  const char* hs = strstr(url, "://");
  hs = hs ? hs + 3 : url;
  size_t n = strcspn(hs, "/:");
  if (n >= sizeof(host)) n = sizeof(host) - 1;
  memcpy(host, hs, n);
  host[n] = 0;
  IPAddress ip;
  if (!WiFi.hostByName(host, ip)) {
    snprintf(err, errLen, "DNS failed: %s", host);
    return false;
  }
  if (ip == IPAddress(0, 0, 0, 0) || ip == IPAddress(127, 0, 0, 1)) {
    snprintf(err, errLen, "DNS blocked %s (%s)", host, ip.toString().c_str());
    return false;
  }
  if (ipOut) strlcpy(ipOut, ip.toString().c_str(), 16);
  return true;
}

// The TLS serialization lock. Function-local static so initialization is
// thread-safe (magic statics); the 15 s cap means a wedged holder degrades to
// the old concurrent behavior instead of deadlocking everyone.
#if defined(ESP8266)
NetTlsGuard::NetTlsGuard() : held_(false) {}
NetTlsGuard::~NetTlsGuard() {}
#else
static SemaphoreHandle_t tlsMux() {
  static SemaphoreHandle_t mux = xSemaphoreCreateMutex();
  return mux;
}
NetTlsGuard::NetTlsGuard()
    : held_(xSemaphoreTake(tlsMux(), pdMS_TO_TICKS(15000)) == pdTRUE) {}
NetTlsGuard::~NetTlsGuard() {
  if (held_) xSemaphoreGive(tlsMux());
}
#endif

// ---------------------------------------------------------------------------
// WiFi backing.
static NetFetchResult fetchOverWifi(const char* url, bool post, const char* headers,
                                    const uint8_t* body, uint16_t bodyLen,
                                    NetFetchSink sink, void* ctx, uint32_t timeoutMs) {
  NetFetchResult r = {};
  r.viaTether = false;

  const bool tls = strncmp(url, "https://", 8) == 0;

  char ips[16];
  if (!netDnsPrecheck(url, r.error, sizeof(r.error), ips)) return r;

  NetTlsGuard tlsLock;   // one TLS session at a time, across threads
  WiFiClientSecure secure;
  WiFiClient plain;
  if (tls) {
    secure.setInsecure();
    secure.setTimeout(timeoutMs / 1000 + 1);
  }

  HTTPClient http;
  // Ask HTTPClient to keep the Transfer-Encoding header: a chunked body read
  // raw off the stream still carries its size lines, and they must be undone
  // below or the caller parses framing instead of payload.
  static const char* kKeep[] = {"Transfer-Encoding"};
  http.collectHeaders(kKeep, 1);
  const bool began = tls ? http.begin(secure, url) : http.begin(plain, url);
  if (!began) {
    strlcpy(r.error, "could not open the connection", sizeof(r.error));
    return r;
  }
  http.setTimeout(timeoutMs);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  // Headers arrive as one "Name: value\n" blob, the same shape the tether uses,
  // so callers write them once and both routes understand them.
  if (headers && headers[0]) {
    String h(headers);
    int start = 0;
    while (start < (int)h.length()) {
      int nl = h.indexOf('\n', start);
      if (nl < 0) nl = h.length();
      const String line = h.substring(start, nl);
      const int colon = line.indexOf(':');
      if (colon > 0)
        http.addHeader(line.substring(0, colon), line.substring(colon + 1));
      start = nl + 1;
    }
  }

  r.status = post ? http.POST((uint8_t*)body, bodyLen) : http.GET();
  if (r.status <= 0) {
    // Everything a remote diagnosis needs on one line: which address the
    // resolver gave us (a filter's sinkhole answers then refuses 443), the
    // largest free heap block (TLS wants big contiguous buffers), and the
    // TLS layer's own error code when there is one.
#if defined(ESP8266)
    snprintf(r.error, sizeof(r.error), "connect failed (%d) @%s b=%uk",
             r.status, ips, (unsigned)(platformMaxFreeBlock() / 1024));
#else
    char sb[4];
    const int ssl = tls ? secure.lastError(sb, sizeof(sb)) : 0;
    snprintf(r.error, sizeof(r.error), "connect failed (%d) @%s b=%uk ssl=%X",
             r.status, ips, (unsigned)(platformMaxFreeBlock() / 1024),
             (unsigned)(ssl < 0 ? -ssl : ssl));
#endif
    http.end();
    return r;
  }

  // The same patient read the Spotify poll needed: Stream's own timeout gives
  // up the moment a TLS record is late, and the caller then sees a truncated
  // body on a response that was merely slow.
  WiFiClient* st = http.getStreamPtr();
  const uint32_t deadline = millis() + timeoutMs;
  int32_t remaining = http.getSize();          // -1 when chunked
  const bool chunked =
      http.header("Transfer-Encoding").indexOf("chunked") >= 0;
  NetChunkDec chunkDec;
  uint8_t buf[512];
  bool abandoned = false;
  struct SinkCtx {
    NetFetchSink sink;
    void* ctx;
    bool* abandoned;
  } sctx = {sink, ctx, &abandoned};
  auto emit = [](void* c, const uint8_t* p, uint16_t n) {
    SinkCtx* s = (SinkCtx*)c;
    if (!*s->abandoned && s->sink && !s->sink(s->ctx, p, n))
      *s->abandoned = true;
    return true;
  };
  while (remaining != 0 && (int32_t)(millis() - deadline) < 0) {
    const int avail = st->available();
    if (avail <= 0) {
      if (!st->connected()) break;
      delay(2);
      continue;
    }
    int want = avail > (int)sizeof(buf) ? (int)sizeof(buf) : avail;
    if (remaining > 0 && remaining < want) want = remaining;
    const int got = st->readBytes(buf, want);
    if (got <= 0) break;
    r.bytes += got;
    if (remaining > 0) remaining -= got;
    if (chunked) {
      if (!netChunkFeed(chunkDec, buf, got, emit, &sctx)) break;  // 0-chunk seen
    } else if (!abandoned && sink && !sink(ctx, buf, (uint16_t)got)) {
      abandoned = true;
    }
  }

  http.end();
  r.ok = (r.status >= 200 && r.status < 300);
  if (!r.ok) snprintf(r.error, sizeof(r.error), "HTTP %d", r.status);
  return r;
}

// ---------------------------------------------------------------------------
#if WITH_TETHER
static NetFetchResult fetchOverTether(const char* url, bool post, const char* headers,
                                      const uint8_t* body, uint16_t bodyLen,
                                      NetFetchSink sink, void* ctx, uint32_t timeoutMs) {
  const TetherResult t = tetherFetch(url, post, headers, body, bodyLen,
                                     (TetherSink)sink, ctx, timeoutMs);
  NetFetchResult r = {};
  r.viaTether = true;
  r.status = t.status;
  r.bytes = t.bytes;
  r.ok = t.ok && t.status >= 200 && t.status < 300;
  if (!r.ok) {
    if (t.error[0]) strlcpy(r.error, t.error, sizeof(r.error));
    else            snprintf(r.error, sizeof(r.error), "HTTP %d", t.status);
  }
  return r;
}
#endif

// ---------------------------------------------------------------------------
NetFetchResult netFetch(const char* url, bool post, const char* headers,
                        const uint8_t* body, uint16_t bodyLen,
                        NetFetchSink sink, void* ctx, uint32_t timeoutMs) {
#if WITH_TETHER
  // The cable wins when it is there. It is the route the user deliberately set
  // up, and on the networks where a tether gets used at all, WiFi is the one
  // that does not work.
  if (tetherActive())
    return fetchOverTether(url, post, headers, body, bodyLen, sink, ctx, timeoutMs);
#endif
  if (!netConnected()) {
    NetFetchResult r = {};
    strlcpy(r.error, "no network and no tether", sizeof(r.error));
    return r;
  }
  return fetchOverWifi(url, post, headers, body, bodyLen, sink, ctx, timeoutMs);
}

// ---------------------------------------------------------------------------
namespace {
struct StringSink {
  String*  out;
  uint32_t cap;
  bool     truncated;
};

bool appendToString(void* ctx, const uint8_t* data, uint16_t len) {
  StringSink* s = (StringSink*)ctx;
  if (s->out->length() + len > s->cap) {
    s->truncated = true;
    return false;                   // stop the transfer; the caller will report it
  }
  s->out->concat((const char*)data, len);
  return true;
}
}  // namespace

NetFetchResult netFetchToString(const char* url, bool post, const char* headers,
                                const uint8_t* body, uint16_t bodyLen,
                                String& out, uint32_t maxBytes, uint32_t timeoutMs) {
  out = "";
  out.reserve(maxBytes > 2048 ? 2048 : maxBytes);
  StringSink s{&out, maxBytes, false};
  NetFetchResult r = netFetch(url, post, headers, body, bodyLen,
                              appendToString, &s, timeoutMs);
  if (s.truncated && r.ok) {
    r.ok = false;
    snprintf(r.error, sizeof(r.error), "response over %u bytes", (unsigned)maxBytes);
  }
  return r;
}
