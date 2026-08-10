// Host-side checks for src/features/calendar/CalendarTime.h.
//
// The centrepiece is not a list of hand-picked dates but a sweep: a hundred
// years of timestamps are formatted by the C library and parsed by our code,
// and the two must agree everywhere. Hand-rolled calendar arithmetic is right
// for every date its author thought of and wrong for one leap-year boundary
// they did not; the sweep does not care what anyone thought of.
#include <cstdio>
#include <cstring>
#include <ctime>

#include "../../src/features/calendar/CalendarTime.h"

static int failures = 0;
static void ck(bool cond, const char* what) {
  printf("  %-5s %s\n", cond ? "ok" : "FAIL", what);
  if (!cond) failures++;
}

static int64_t parse(const char* s, int assume = 0) {
  int64_t v = 0;
  bool ad = false;
  if (!calIsoToUtc(s, assume, &v, &ad)) return INT64_MIN;
  return v;
}

int main() {
  printf("--- against the C library, 1970 to 2070 ----------------------\n");
  {
    // Steps of a prime number of seconds so the sweep lands on every hour,
    // minute and second value, either side of every month boundary.
    bool agree = true;
    long long checked = 0;
    char buf[40];
    for (int64_t t = 0; t < 3155760000LL /*~2070*/; t += 990017) {
      time_t tt = (time_t)t;
      struct tm g;
      gmtime_r(&tt, &g);
      snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
               g.tm_year + 1900, g.tm_mon + 1, g.tm_mday,
               g.tm_hour, g.tm_min, g.tm_sec);
      if (parse(buf) != t) {
        if (agree) printf("        first disagreement at %s\n", buf);
        agree = false;
      }
      checked++;
    }
    printf("        %lld timestamps compared\n", checked);
    ck(agree, "our epoch math matches timegm everywhere it was asked");
  }

  printf("\n--- the shapes the two providers actually send ---------------\n");
  ck(parse("2026-08-10T14:30:00-07:00") == parse("2026-08-10T21:30:00Z"),
     "a Google offset timestamp equals its UTC twin");
  ck(parse("2026-08-10T21:30:00.0000000", 0) == parse("2026-08-10T21:30:00Z"),
     "a Graph fractional-seconds timestamp parses, assumed UTC");
  ck(parse("2026-08-10T14:30:00+05:30") == parse("2026-08-10T09:00:00Z"),
     "a half-hour zone (India) lands where it should");
  ck(parse("2026-08-10T14:30:00+0530") == parse("2026-08-10T09:00:00Z"),
     "with or without the colon in the offset");
  {
    // All-day: a bare date is local midnight, so the assumed offset applies.
    int64_t v = 0;
    bool allDay = false;
    ck(calIsoToUtc("2026-08-11", -7 * 60, &v, &allDay) &&
           allDay && v == parse("2026-08-11T07:00:00Z"),
       "an all-day date is flagged and lands at local midnight");
    calIsoToUtc("2026-08-11T09:00:00Z", 0, &v, &allDay);
    ck(!allDay, "a timed event is not flagged all-day");
  }
  {
    // An offset crossing midnight moves the civil date.
    ck(parse("2026-01-01T00:30:00+01:00") == parse("2025-12-31T23:30:00Z"),
       "an offset can carry an event into the previous year");
  }
  ck(parse("2026-06-30T23:59:60Z") == parse("2026-06-30T23:59:59Z"),
     "a leap second clamps instead of overflowing the minute");

  printf("\n--- what must not parse --------------------------------------\n");
  ck(parse("") == INT64_MIN, "empty string");
  ck(parse("2026-13-01") == INT64_MIN, "month 13");
  ck(parse("2026-08-32") == INT64_MIN, "day 32");
  ck(parse("2026-08-10T25:00:00Z") == INT64_MIN, "hour 25");
  ck(parse("2026-08-10T14:61:00Z") == INT64_MIN, "minute 61");
  ck(parse("2026-8-10") == INT64_MIN, "unpadded month");
  ck(parse("2026-08-10T14:30:00Zjunk") == INT64_MIN, "trailing junk");
  ck(parse("2026-08-10T14:30:00+15:00") == INT64_MIN, "offset beyond +14");
  ck(parse("not a date") == INT64_MIN, "prose");

  printf("\n--- the countdown --------------------------------------------\n");
  {
    char b[24];
    calCountdown(61, 3600, b, sizeof b);
    ck(!strcmp(b, "in 2m"), "61 seconds away rounds up to 2m, not down to 1m");
    calCountdown(60, 3600, b, sizeof b);
    ck(!strcmp(b, "in 1m"), "exactly a minute is 1m");
    calCountdown(59 * 60, 9999, b, sizeof b);
    ck(!strcmp(b, "in 59m"), "59 minutes stays in minutes");
    calCountdown(60 * 60, 9999, b, sizeof b);
    ck(!strcmp(b, "in 1h 00m"), "an hour flips to h/m");
    calCountdown(5025 * 60, 999999, b, sizeof b);
    ck(!strcmp(b, "in 3d"), "days once it is more than a day");
    calCountdown(-60, 1800, b, sizeof b);
    ck(!strcmp(b, "now"), "started but not over is now");
    calCountdown(-3600, -60, b, sizeof b);
    ck(!strcmp(b, "over"), "ended is over");
  }

  printf("\n-------------------------------------------------------------\n");
  if (failures) { printf("%d check(s) FAILED\n", failures); return 1; }
  printf("all checks passed\n");
  return 0;
}
