// NetFetch.h — one way to ask for a URL, whichever route is available.
//
// Every network client here grew its own copy of "make a WiFiClientSecure, make
// an HTTPClient, set the timeouts, check the status". That was tolerable while
// WiFi was the only route. With a USB tether as a second one it would mean two
// of everything, and the second copy is exactly where the bugs would live.
//
// So: callers say what they want, not how to get it. When a tether is up the
// request goes down the cable; otherwise it goes over WiFi. Nothing above this
// line knows which, and neither path is a special case of the other.
#pragma once
#include "config.h"
#include <Arduino.h>

// Called with each chunk of the body as it arrives. Return false to stop early;
// the transfer is abandoned and the result reports what arrived up to then.
typedef bool (*NetFetchSink)(void* ctx, const uint8_t* data, uint16_t len);

struct NetFetchResult {
  bool     ok;         // the request completed and the status was 2xx
  int      status;     // HTTP status, 0 if it never got that far
  uint32_t bytes;      // body bytes handed to the sink
  bool     viaTether;  // which route carried it, for diagnostics
  char     error[72];  // why not, when !ok
};

// One request. Blocking.
//   headers  "Name: value\n" separated; may be null
//   body     null for a GET
//   sink     may be null to discard the body (a HEAD-alike)
NetFetchResult netFetch(const char* url, bool post, const char* headers,
                        const uint8_t* body, uint16_t bodyLen,
                        NetFetchSink sink, void* ctx, uint32_t timeoutMs);

// Convenience: collect the whole body into a String, bounded. Returns the same
// result; `out` holds what arrived. For responses small enough to hold at once,
// which is every JSON payload on this device.
NetFetchResult netFetchToString(const char* url, bool post, const char* headers,
                                const uint8_t* body, uint16_t bodyLen,
                                String& out, uint32_t maxBytes, uint32_t timeoutMs);

// Resolve a URL's host and reject filtering-resolver block answers (0.0.0.0 /
// 127.0.0.1) before a fetch. Returns false with `err` filled when DNS is the
// problem — which on hotspots and hotel WiFi it often is.
bool netDnsPrecheck(const char* url, char* err, size_t errLen,
                    char* ipOut = nullptr);   // ipOut: >=16 bytes, dotted quad

// Serialize TLS fetches across threads. The calendar task and the display
// loop each stand up a TLS session with ~20+ KB of buffers; two at once on a
// fragmenting heap is a "connection refused despite plenty of total heap"
// generator. Scoped: construct to take the lock, destruct to release. On the
// ESP8266 (single-threaded) it is a no-op.
class NetTlsGuard {
 public:
  NetTlsGuard();
  ~NetTlsGuard();
 private:
  bool held_;
};

// True when a request has somewhere to go at all: a joined network, or a
// tether. Anything gating work on connectivity should ask this rather than the
// radio — the radio is one of two answers, and treating it as the only one is
// what left Spotify silent on a cube that had a perfectly good cable.
bool netHaveRoute();

// Which route the next call would take. Worth surfacing: "the ticker is blank"
// has a very different cause on each.
bool netFetchTethered();
