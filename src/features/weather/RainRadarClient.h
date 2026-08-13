// RainRadarClient.h — the last hour of rain, fetched and held for the screen.
//
// RainViewer publishes a tile pyramid of the world's radar composites every
// ten minutes (past ~2 h, plus a short nowcast when their model is running),
// free and keyless, CORS-open — so the same fetches work over WiFi and down
// the tether cable. Esri's street-map tiles are baseline JPEG, which this
// chip decodes from ROM; the radar tiles are PNG, which it inflates with the
// ROM copy of miniz and defilters by hand (RainRadar.h holds that logic).
//
// Memory is the design constraint, so nothing full-size is ever kept: the map
// lands as 128x128 RGB332 at half brightness (16 KB), each radar frame as a
// 64x64 grid of intensity nibbles (2 KB), and everything bigger lives only for
// the seconds one tile takes to decode. The whole build runs on the weather
// task; the display loop reads a locked view and draws.
//
// The cheap-gate rule the user actually asked for: every cycle fetches ONE
// tile (the newest) first, and only when it shows real precipitation does the
// full map + history + nowcast build happen. A dry week costs six tiny tiles
// an hour; the animation only ever exists when there is something to animate.
#pragma once
#include "config.h"
#if WITH_WEATHER

#include <Arduino.h>
#include "RainRadar.h"

struct RainRadarView {
  uint8_t        frames;                 // total frames held, oldest first
  uint8_t        nowIdx;                 // index of the newest *observed* frame
  int16_t        minOff[RR_FRAMES_MAX];  // minutes relative to that frame
  uint8_t        markerX, markerY;       // the location, in tile pixels
  const uint8_t* map;                    // RR_MAP_PX^2 RGB332, pre-dimmed
  const uint8_t* grid[RR_FRAMES_MAX];    // nibble planes, RR_GRID_BYTES each
};

// Task-side. Call freely from the weather task's loop; it times itself (ten
// minutes between looks, sooner after a failure) and tears down when disabled.
void rainRadarCycle(float lat, float lon, bool enabled);

// True when a built animation is ready to draw. Cheap; any thread.
bool rainRadarReady();

// Lock and fill the view. Returns false (nothing locked) when not ready.
// Balance every true return with rainRadarRelease() promptly — the weather
// task waits on this lock to publish a rebuild.
bool rainRadarAcquire(RainRadarView& v);
void rainRadarRelease();

// The last cycle's outcome, for status surfaces: "radar: 9 frames",
// "radar quiet", or why not.
const char* rainRadarNote();

#endif  // WITH_WEATHER
