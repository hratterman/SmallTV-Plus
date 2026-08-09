// Captive.h — noticing, and getting past, a network that has let you associate
// but not out.
//
// Associating with an access point is not the same as having internet, and this
// firmware used to treat them as the same thing: netConnected() means "the radio
// has an IP", which on a hotel or conference network is true while every fetch
// times out. The device shows a network name, an IP address and a signal
// strength, and every feature quietly fails. There is nothing on the screen or
// in /api/status that says why.
//
// So: probe for a real route out, and when the answer is a portal, try to accept
// its terms the way a browser would. The probe is the valuable half — it is
// generic and always correct. The auto-accept is best effort, because portals
// range from one hidden form to a single-page app, and no embedded device is
// going to run the second kind.
#pragma once
#include "config.h"
#if WITH_CAPTIVE

#include <Arduino.h>
#include "Settings.h"

enum CaptiveState : uint8_t {
  CAP_UNKNOWN = 0,   // not probed yet since the last association
  CAP_ONLINE,        // the probe came back clean: there is a route out
  CAP_PORTAL,        // something answered instead of the probe target
  CAP_NO_NET,        // the probe could not reach anything at all
};

void         captiveBegin(const Settings& s);
void         captiveService(const Settings& s);   // self-paced; call from the loop
CaptiveState captiveStateNow();
bool         captiveOnline();          // CAP_ONLINE — the only state that means working
const char*  captiveDetail();          // human-readable last result
const char*  captivePortalUrl();       // where the portal answered from, "" if none
uint8_t      captiveAttempts();        // accept attempts since association
void         captiveForceTry();        // web UI / long-press: probe and accept now

// True once the device has decided it is stuck behind a portal it cannot pass.
// Net.cpp raises its own AP alongside the station in that case, because a device
// that cannot reach the internet *and* cannot be reached on the hostile network
// has no way left to be told what to do.
bool captiveStuck();

#endif  // WITH_CAPTIVE
