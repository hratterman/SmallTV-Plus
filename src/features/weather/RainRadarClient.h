// RainRadarClient.h — the last hour of rain, kept on flash for a 55 KB heap.
//
// RainViewer publishes a tile pyramid of the world's radar composites every
// ten minutes (past ~2 h, plus a short nowcast when their model is running),
// free and keyless — and, measured: over plain HTTP as well as TLS, which on
// WiFi removes the 45 KB TLS arena from the radar's account entirely. Esri's
// street-map tiles are baseline JPEG (ROM TJpgDec); the radar tiles are PNG,
// inflated with the ROM copy of miniz and defiltered by hand (RainRadar.h).
//
// The first version cached everything in RAM and needed ~110 KB at peak; a
// field cube running the miner reported 55 KB free, which is not a tuning
// problem. So nothing big lives in RAM at all now:
//   - a fetched tile streams straight into a LittleFS file (no buffer),
//   - the inflater runs afterwards, input read back from flash — the one
//     large transient (~47 KB), never concurrent with a connection,
//   - each decoded frame is a 2 KB grid file keyed by its timestamp, so a
//     steady rain fetches only the one new frame per cycle, and a dry sky
//     writes nothing to flash at all,
//   - the renderer streams the map and the current grid from flash as it
//     draws. Steady-state RAM for the whole feature: about 2 KB.
//
// The cheap-gate rule stands: every cycle examines the newest frame first,
// and only real precipitation triggers the full build.
#pragma once
#include "config.h"
#if WITH_WEATHER

#include <Arduino.h>
#include "RainRadar.h"

// Metadata only — pixel data is read through the accessors below, which
// stream it from flash while the acquire lock is held.
struct RainRadarView {
  uint8_t frames;                 // total frames held, oldest first
  uint8_t nowIdx;                 // index of the newest *observed* frame
  int16_t minOff[RR_FRAMES_MAX];  // minutes relative to that frame
  uint8_t markerX, markerY;       // the location, in tile pixels
};

// Task-side. Call freely from the weather task's loop; it times itself and
// tears down (files included) when disabled or the location moves.
void rainRadarCycle(float lat, float lon, bool enabled);

bool rainRadarReady();            // cheap hint, any thread
const char* rainRadarNote();      // last outcome, for status surfaces

// Lock + metadata. Balance every true return with rainRadarRelease().
bool rainRadarAcquire(RainRadarView& v);
void rainRadarRelease();

// While holding the lock: copy one frame's grid (RR_GRID_BYTES) into `out`,
// and stream the base map a row at a time (RR_MAP_PX bytes of RGB332 per
// row). mapBegin/mapEnd bracket one drawing pass; rows may repeat or skip.
bool rainRadarReadGrid(uint8_t frameIdx, uint8_t* out);
bool rainRadarMapBegin();
bool rainRadarMapRow(int mapRow, uint8_t* out);
void rainRadarMapEnd();

#endif  // WITH_WEATHER
