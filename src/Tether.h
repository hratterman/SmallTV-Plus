// Tether.h — internet over the USB cable, for networks the cube cannot join.
//
// An office with 802.1X, a portal that needs a browser, a guest network that
// will not take a device without one: in all of them the cube is stuck and the
// computer next to it is not. This lets it borrow that computer's connection.
// Run tools/tether.py there; nothing has to be configured on either side.
//
// Design notes worth keeping:
//
//  * There is no dedicated task. Whoever wants data pumps the link themselves
//    while they wait, under a mutex, and the main loop pumps it when idle. A
//    reader task would need to hand chunks across to a blocked requester, and
//    that plumbing is all cost and no benefit when only one request is ever in
//    flight.
//
//  * The link is discovered, not configured. The cube announces itself every
//    couple of seconds; if a host answers, the tether is up. Stop hearing from
//    it and the cube goes back to WiFi on its own. Unplugging is not an error.
//
//  * The UART is shared with the debug log, and another task can print in the
//    middle of a frame being written. Framing plus a CRC means the host drops
//    the damaged frame rather than misreading it, and the request times out and
//    is retried. Losing the log while tethered would be a much worse trade.
#pragma once
#include "config.h"
#if WITH_TETHER

#include <Arduino.h>

void tetherBegin();

// Settings over the cable. Tether.cpp owns the framing; these two are supplied
// by the application because Tether has no business knowing what a Setting is.
//   serialise: write the current settings as JSON into `out`
//   apply:     take JSON, apply and persist it; return an error or nullptr
// A calendar file pushed down the cable (SF_ICS_DATA/END). feed() receives the
// bytes as they arrive; done() finishes the parse and returns a short status
// string for the host ("ok: 3 events" / why not). Registered by main.cpp only
// when the calendar feature is compiled in.
void tetherOnIcs(void (*feed)(const uint8_t* data, uint16_t len),
                 const char* (*done)());

void tetherOnConfig(void (*serialise)(String& out),
                    const char* (*apply)(const String& json));

// Pump the link: send the periodic hello, take delivery of the clock, notice
// the host going away. Call from the main loop; costs nothing when idle.
void tetherService();

// True while a host has answered recently. Everything else keys off this.
bool tetherActive();

// Seconds since the host was last heard from, for /api/status.
uint32_t tetherIdleSec();

// Each chunk of the response body, as it arrives. Return false to abandon the
// transfer — the album art decoder does this when it has seen enough.
typedef bool (*TetherSink)(void* ctx, const uint8_t* data, uint16_t len);

struct TetherResult {
  bool     ok;          // the host performed the request and the body completed
  int      status;      // HTTP status, 0 if the request never got that far
  uint32_t bytes;       // body bytes delivered
  char     error[48];   // why not, when !ok
};

// Perform one request over the cable. Blocking, and serialised: only one is in
// flight at a time, which is all this device has ever needed.
//   headers: "Name: value\n" separated, may be empty
//   body:    nullptr for a GET
TetherResult tetherFetch(const char* url, bool post,
                         const char* headers,
                         const uint8_t* body, uint16_t bodyLen,
                         TetherSink sink, void* ctx,
                         uint32_t timeoutMs);

#endif  // WITH_TETHER
