// CalendarClient.h — fetching the next couple of days from Google or Outlook.
//
// The OAuth shape is Spotify's: the user registers their own client, obtains a
// refresh token once with tools/calendar_auth.py, and the cube trades it for
// short-lived access tokens by itself from then on. Both providers' APIs and
// token endpoints are CORS-clean (measured; docs/tether-limits.md), so all of
// this works identically over WiFi and over the tether.
//
// The poll runs on its own task, like Spotify's and for the same reason: a TLS
// handshake takes long enough to cost the display visible frames, and the main
// loop is where the display lives. Everything the loop reads comes out as a
// snapshot copied under a lock.
//
// One provider difference leaks out: Microsoft rotates refresh tokens on use.
// The task keeps the newest one and main.cpp persists it via
// calendarTakeRotatedToken() — the client cannot write settings itself.
#pragma once
#include "config.h"
#if WITH_CALENDAR

#include <Arduino.h>
#include "Settings.h"

struct CalEvent {
  char    title[CAL_TITLE_LEN];
  int64_t startUtc;
  int64_t endUtc;
  bool    allDay;
};

struct CalSnapshot {
  CalEvent events[CAL_MAX_EVENTS];
  uint8_t  count;
  bool     ok;          // at least one successful fetch since (re)configuring
  uint32_t ageMs;       // since that fetch
  char     error[64];   // last failure, "" when none
};

void calendarInit(const Settings& s);       // start/refresh config; spawns the task
void calendarSnapshot(CalSnapshot& out);    // consistent copy for the renderer

// Index of the first event in `s` still in progress or ahead of `nowUtc`,
// or -1. Pure so the mode and anything else agree on what "next" means.
static inline int calSnapshotNext(const CalSnapshot& s, int64_t nowUtc) {
  for (uint8_t i = 0; i < s.count; i++)
    if (s.events[i].endUtc > nowUtc) return i;
  return -1;
}

// Microsoft handed back a replacement refresh token; empty when none pending.
// Reading it clears it, so a persisted token is not persisted twice. The
// device-code link below delivers its token through the same channel.
String calendarTakeRotatedToken();

// ---- on-device linking (Microsoft only) -------------------------------------
// The device-code flow needs no browser on the cube and no secret anywhere:
// the cube asks Microsoft for a short code, shows it, and polls until the user
// has typed it into microsoft.com/devicelogin on any machine. Google's device
// flow does not offer calendar scopes, so Google keeps the helper script.
enum CalLinkPhase : uint8_t {
  CAL_LINK_IDLE = 0,
  CAL_LINK_STARTING,   // asking Microsoft for a code
  CAL_LINK_CODE,       // code issued; waiting for the user to enter it
  CAL_LINK_DONE,       // refresh token obtained and handed to main.cpp
  CAL_LINK_FAILED,
};

struct CalLinkState {
  CalLinkPhase phase;
  char code[20];       // what the user types
  char url[64];        // where they type it
  char msg[64];        // failure reason
};

void calendarLinkStart();               // uses the configured client id
void calendarLinkState(CalLinkState& out);

#endif  // WITH_CALENDAR
