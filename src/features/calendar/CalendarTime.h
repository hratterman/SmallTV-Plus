// CalendarTime.h — turning what a calendar API says into seconds, and back
// into words.
//
// Pure arithmetic, no Arduino: everything here is host-tested against the C
// library's own timegm() across a century of dates, because hand-rolled
// calendar math is famously right for every date you tried and wrong for
// March 1st of some leap year you did not.
//
// It exists because the two providers hand back three different shapes:
//   Google timed event:  "2026-08-10T14:30:00-07:00"   (offset always present)
//   Graph timed event:   "2026-08-10T21:30:00.0000000" (no offset; the request
//                        asks for UTC via a Prefer header, so UTC is assumed)
//   all-day, both:       "2026-08-10"                  (no time at all; it
//                        means local midnight, so the caller supplies the
//                        cube's own UTC offset)
// One parser covers all three rather than three ad-hoc ones drifting apart.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

// Days since 1970-01-01 from a civil date. Howard Hinnant's algorithm,
// bit-for-bit the one documented at howardhinnant.github.io/date_algorithms.
static inline int64_t calDaysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  const int      era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

struct CalIso {
  int  y, mo, d;
  int  h, mi, s;
  int  offMin;      // parsed zone offset, minutes east of UTC
  bool hasTime;     // false: a bare date (an all-day event)
  bool hasOff;      // false: the string carried no zone at all
};

// Reads "YYYY-MM-DD", optionally "Thh:mm[:ss[.frac]]", optionally "Z" or
// "+hh:mm"/"-hhmm". Returns false on anything else — a false here becomes a
// skipped event, never a garbage timestamp.
static inline bool calParseIso(const char* p, CalIso& t) {
  if (!p) return false;
  const char* q = p;
  auto num = [&q](int digits, int* out) -> bool {
    int v = 0;
    for (int i = 0; i < digits; i++) {
      if (*q < '0' || *q > '9') return false;
      v = v * 10 + (*q++ - '0');
    }
    *out = v;
    return true;
  };

  t.h = t.mi = t.s = 0;
  t.offMin = 0;
  t.hasTime = t.hasOff = false;

  if (!num(4, &t.y) || *q++ != '-') return false;
  if (!num(2, &t.mo) || *q++ != '-') return false;
  if (!num(2, &t.d)) return false;
  if (t.mo < 1 || t.mo > 12 || t.d < 1 || t.d > 31) return false;

  if (*q == 'T' || *q == 't') {
    q++;
    t.hasTime = true;
    if (!num(2, &t.h) || *q++ != ':') return false;
    if (!num(2, &t.mi)) return false;
    if (*q == ':') {
      q++;
      if (!num(2, &t.s)) return false;
    }
    if (t.h > 23 || t.mi > 59 || t.s > 60) return false;   // 60: leap second, clamp later
    if (*q == '.') {                                        // Graph's ".0000000"
      q++;
      if (*q < '0' || *q > '9') return false;
      while (*q >= '0' && *q <= '9') q++;
    }
  }

  if (*q == 'Z' || *q == 'z') {
    q++;
    t.hasOff = true;
    t.offMin = 0;
  } else if (*q == '+' || *q == '-') {
    const bool west = (*q == '-');
    q++;
    int oh, om = 0;
    if (!num(2, &oh)) return false;
    if (*q == ':') q++;
    if (*q >= '0' && *q <= '9') {
      if (!num(2, &om)) return false;
    }
    if (oh > 14 || om > 59) return false;
    t.hasOff = true;
    t.offMin = (oh * 60 + om) * (west ? -1 : 1);
  }
  return *q == 0;
}

// The string as seconds since the epoch, UTC. Strings carrying their own zone
// use it; naked ones use assumeOffMin (0 for Graph-in-UTC, the cube's local
// offset for all-day dates). A leap-second :60 clamps to :59 rather than
// spilling into the next minute.
static inline bool calIsoToUtc(const char* p, int assumeOffMin, int64_t* out,
                               bool* allDay) {
  CalIso t;
  if (!calParseIso(p, t)) return false;
  const int sec = t.s > 59 ? 59 : t.s;
  const int off = t.hasOff ? t.offMin : assumeOffMin;
  *out = calDaysFromCivil(t.y, t.mo, t.d) * 86400LL +
         t.h * 3600LL + t.mi * 60LL + sec - (int64_t)off * 60;
  if (allDay) *allDay = !t.hasTime;
  return true;
}

// "in 5m" / "in 1h 05m" / "in 3d" / "now". startDelta and endDelta are
// event-start minus now and event-end minus now. Minutes round up: a meeting
// 61 seconds away is "in 2m", because "in 1m" reads as already-too-late.
static inline void calCountdown(int64_t startDelta, int64_t endDelta,
                                char* out, size_t n) {
  if (endDelta <= 0) { snprintf(out, n, "over"); return; }
  if (startDelta <= 0) { snprintf(out, n, "now"); return; }
  const int64_t m = (startDelta + 59) / 60;
  if (m < 60)        snprintf(out, n, "in %dm", (int)m);
  else if (m < 1440) snprintf(out, n, "in %dh %02dm", (int)(m / 60), (int)(m % 60));
  else               snprintf(out, n, "in %dd", (int)(m / 1440));
}
