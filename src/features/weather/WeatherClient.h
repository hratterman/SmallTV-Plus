// WeatherClient.h — Open-Meteo current conditions + short forecast.
//
// No API key, no account: api.open-meteo.com answers anonymously and sends
// Access-Control-Allow-Origin: * (measured), so the same fetch works on WiFi
// and down the tether cable. The poll runs on its own task, like Spotify's
// and the calendar's: a TLS handshake on the display loop froze the whole UI
// — touch included — for its full duration, which is a lot of dead taps.
#pragma once
#include "config.h"
#if WITH_WEATHER

#include <Arduino.h>
#include "Settings.h"
#include "WeatherLogic.h"

#define WX_DAYS 4                     // today + three ahead

struct WeatherData {
  bool     valid;                     // ever fetched OK
  bool     error;                     // last fetch failed
  char     errMsg[72];
  uint32_t lastOkMs;
  float    curTemp;
  uint8_t  curCode;                   // WMO code, current conditions
  bool     day;                       // is_day: sun vs moon for WX_CLEAR
  float    hi[WX_DAYS], lo[WX_DAYS];  // [0] is today
  uint8_t  code[WX_DAYS];
  int8_t   dow[WX_DAYS];              // 0=Sun, -1 unknown
};

void weatherInit(const Settings& s);      // config hand-off; starts the task
void weatherSnapshot(WeatherData& out);   // a consistent copy, any thread

#endif  // WITH_WEATHER
