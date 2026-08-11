// Host-side checks for the weather mode's pure logic (WeatherLogic.h): the
// WMO-code grouping every icon draw goes through, and the forecast column's
// day-of-week arithmetic.
#include <cstdio>
#include <cstring>

#include "../../src/features/weather/WeatherLogic.h"

static int failures = 0;
static void ck(bool cond, const char* what) {
  printf("  %-5s %s\n", cond ? "ok" : "FAIL", what);
  if (!cond) failures++;
}

int main() {
  printf("--- WMO code grouping ----------------------------------------\n");
  ck(wxClass(0) == WX_CLEAR, "0 is clear");
  ck(wxClass(1) == WX_PARTLY && wxClass(2) == WX_PARTLY, "1-2 partly cloudy");
  ck(wxClass(3) == WX_CLOUD, "3 overcast");
  ck(wxClass(45) == WX_FOG && wxClass(48) == WX_FOG, "45/48 fog");
  ck(wxClass(51) == WX_RAIN && wxClass(55) == WX_RAIN, "drizzle is rain");
  ck(wxClass(56) == WX_RAIN && wxClass(57) == WX_RAIN, "freezing drizzle too");
  ck(wxClass(61) == WX_RAIN && wxClass(65) == WX_RAIN && wxClass(67) == WX_RAIN,
     "rain, heavy rain, freezing rain");
  ck(wxClass(71) == WX_SNOW && wxClass(75) == WX_SNOW && wxClass(77) == WX_SNOW,
     "snow and snow grains");
  ck(wxClass(80) == WX_RAIN && wxClass(82) == WX_RAIN, "showers are rain");
  ck(wxClass(85) == WX_SNOW && wxClass(86) == WX_SNOW, "snow showers are snow");
  ck(wxClass(95) == WX_STORM && wxClass(96) == WX_STORM && wxClass(99) == WX_STORM,
     "95-99 thunderstorm");
  {
    // Every possible byte maps to a drawable class — no icon index can escape
    // the switch in the renderer.
    bool ok = true;
    for (int c = 0; c <= 255; c++)
      if (wxClass((uint8_t)c) > WX_STORM) ok = false;
    ck(ok, "all 256 code values land on a drawable class");
  }

  printf("\n--- day of week ----------------------------------------------\n");
  ck(wxDowFromDate("2026-08-10") == 1, "2026-08-10 is a Monday");
  ck(wxDowFromDate("2026-08-11") == 2, "2026-08-11 is a Tuesday");
  ck(wxDowFromDate("2026-01-01") == 4, "2026-01-01 is a Thursday");
  ck(wxDowFromDate("2024-02-29") == 4, "2024-02-29 (leap day) is a Thursday");
  ck(wxDowFromDate("2000-01-01") == 6, "2000-01-01 is a Saturday");
  ck(wxDowFromDate("1999-12-31") == 5, "1999-12-31 is a Friday (century edge)");
  ck(wxDowFromDate("") == -1, "empty string is rejected");
  ck(wxDowFromDate("2026-13-01") == -1, "month 13 is rejected");
  ck(wxDowFromDate("garbage-in") == -1, "garbage is rejected");
  ck(wxDowFromDate(nullptr) == -1, "null is rejected");

  printf("\n-------------------------------------------------------------\n");
  if (failures) { printf("%d check(s) FAILED\n", failures); return 1; }
  printf("all checks passed\n");
  return 0;
}
