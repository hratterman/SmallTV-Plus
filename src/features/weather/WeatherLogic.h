// WeatherLogic.h — the pure arithmetic of the weather mode, host-includable.
//
// Everything here is testable without a device (tools/weather_selftest): the
// WMO weather-code grouping the icons draw from, and the day-of-week for the
// forecast columns. The client and the renderer both read this header, so the
// mapping cannot fork between fetch and draw.
#pragma once
#include <stdint.h>

// The icon classes the screen can draw. Open-Meteo reports WMO codes, which
// distinguish far more than five 40x40 pictures can; this is the honest
// grouping of all 100 codes into the pictures that exist.
enum WxClass : uint8_t {
  WX_CLEAR = 0,   // 0: clear sky (sun, or moon at night)
  WX_PARTLY,      // 1-2: mainly clear / partly cloudy
  WX_CLOUD,       // 3: overcast
  WX_FOG,         // 45, 48
  WX_RAIN,        // 51-67 drizzle+rain (freezing included), 80-82 showers
  WX_SNOW,        // 71-77 snow, 85-86 snow showers
  WX_STORM,       // 95-99 thunderstorm
};

static inline WxClass wxClass(uint8_t code) {
  if (code == 0)                                  return WX_CLEAR;
  if (code <= 2)                                  return WX_PARTLY;
  if (code == 3)                                  return WX_CLOUD;
  if (code == 45 || code == 48)                   return WX_FOG;
  if (code >= 51 && code <= 67)                   return WX_RAIN;
  if (code >= 71 && code <= 77)                   return WX_SNOW;
  if (code >= 80 && code <= 82)                   return WX_RAIN;
  if (code == 85 || code == 86)                   return WX_SNOW;
  if (code >= 95)                                 return WX_STORM;
  return WX_CLOUD;                                // unexpected: neutral
}

// Sakamoto's day-of-week. 0 = Sunday. Valid for the Gregorian calendar.
static inline int wxDow(int y, int m, int d) {
  static const int t[12] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (m < 3) y -= 1;
  return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

// "2026-08-11" -> 0..6, or -1 when the string is not a date. Open-Meteo's
// daily.time entries are exactly this shape.
static inline int wxDowFromDate(const char* iso) {
  if (!iso) return -1;
  int y = 0, m = 0, d = 0, i = 0;
  for (; i < 4; i++) { if (iso[i] < '0' || iso[i] > '9') return -1; y = y * 10 + iso[i] - '0'; }
  if (iso[4] != '-') return -1;
  for (i = 5; i < 7; i++) { if (iso[i] < '0' || iso[i] > '9') return -1; m = m * 10 + iso[i] - '0'; }
  if (iso[7] != '-') return -1;
  for (i = 8; i < 10; i++) { if (iso[i] < '0' || iso[i] > '9') return -1; d = d * 10 + iso[i] - '0'; }
  if (m < 1 || m > 12 || d < 1 || d > 31) return -1;
  return wxDow(y, m, d);
}
