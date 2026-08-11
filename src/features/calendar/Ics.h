// Ics.h — reading a calendar's secret iCal feed, including its repeats.
//
// This exists because it is the one truly plug-and-play calendar source: both
// Google and Outlook publish a private URL per calendar, and pasting a URL is
// the entire setup. No OAuth, no client registration, no consent screens. The
// price is paid here instead, because an ICS feed hands over recurring events
// as RRULEs — the rule, not the occurrences — and "your next obligation" is
// usually an occurrence of a rule (the weekly standup, not its first meeting
// in 2019).
//
// So this is a streaming parser plus a bounded RRULE expander. Streaming,
// because Google's feed is the calendar's whole history — years of it, easily
// megabytes — and the ESP32 will never hold that; events are folded into a
// keep-the-soonest list as they stream past. Bounded, because expansion only
// has to answer "what lands in the next 48 hours", which turns the general
// RRULE nightmare into a finite walk.
//
// What is supported, deliberately: FREQ=DAILY/WEEKLY/MONTHLY/YEARLY, INTERVAL,
// COUNT, UNTIL, weekly BYDAY lists, monthly BYMONTHDAY and nth-weekday BYDAY
// (3TU, -1FR), EXDATE, and RECURRENCE-ID overrides including cancellations.
// That covers standups, birthdays, rent day, and "last Friday of the month".
// A rule beyond it (BYSETPOS, BYWEEKNO, ...) is counted in skippedRules()
// rather than silently dropped, so the screen can say repeats were missed.
//
// One honest approximation: events carrying a TZID are treated as being in the
// cube's own timezone. Parsing VTIMEZONE blocks means implementing DST-change
// RRULEs, for the rare case of a personal calendar holding events created in
// a foreign zone. Expansion happens in civil (local) time, so a 14:30 standup
// stays 14:30 across a DST change instead of drifting an hour.
//
// Pure C++, no Arduino: everything here runs on a host under
// tools/ics_selftest, which is where the RRULE cases live.
#pragma once
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "CalendarTime.h"
#include "../../TextFold.h"

// Private bounded copy: Arduino has strlcpy and newer glibc grew one too, with
// just enough declaration differences that owning seven lines beats
// negotiating with both.
static inline void icsCopy(char* dst, const char* src, size_t n) {
  if (!n) return;
  const size_t len = strlen(src);
  const size_t c = len < n - 1 ? len : n - 1;
  memcpy(dst, src, c);
  dst[c] = 0;
}

#define ICS_TITLE_LEN   64
#define ICS_KEEP        8     // soonest events retained while streaming
#define ICS_OVERRIDES   12    // RECURRENCE-ID edits tracked per feed
#define ICS_EXDATES     8     // per event
#define ICS_LINE_MAX    240   // logical line, after unfolding
#define ICS_MAX_STEPS   40000 // expansion walk cap (110 years of a daily rule)

// Inverse of calDaysFromCivil — same source, same conventions.
static inline void calCivilFromDays(int64_t z, int* y, int* m, int* d) {
  z += 719468;
  const int64_t  era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t  yy  = (int64_t)yoe + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp  = (5 * doy + 2) / 153;
  const unsigned dd  = doy - (153 * mp + 2) / 5 + 1;
  const unsigned mm  = mp + (mp < 10 ? 3 : (unsigned)-9);
  *y = (int)(yy + (mm <= 2));
  *m = (int)mm;
  *d = (int)dd;
}

// 0=Sunday..6=Saturday. Day 0 (1970-01-01) was a Thursday.
static inline int icsDow(int64_t day) { return (int)(((day % 7) + 7 + 4) % 7); }

struct IcsOut {
  int64_t start, end;      // UTC epoch seconds
  bool    allDay;
  char    title[ICS_TITLE_LEN];
};

class IcsParser {
 public:
  // assumeOffMin: the cube's local offset, applied to floating/TZID times and
  // all-day dates. winStart/winEnd bound both filtering and expansion.
  void begin(int64_t winStart, int64_t winEnd, int assumeOffMin) {
    winStart_ = winStart;
    winEnd_ = winEnd;
    offMin_ = assumeOffMin;
    count_ = nOver_ = 0;
    skipped_ = 0;
    plen_ = llen_ = 0;
    inEvent_ = inAlarm_ = false;
  }

  void feed(const char* data, unsigned len) {
    for (unsigned i = 0; i < len; i++) {
      const char c = data[i];
      if (c == '\r') continue;
      if (c != '\n') {
        if (plen_ < sizeof(pbuf_) - 1) pbuf_[plen_++] = c;
        continue;
      }
      pbuf_[plen_] = 0;
      // Unfolding: a physical line starting with space or tab continues the
      // previous logical line, so that one cannot be processed until now.
      if (pbuf_[0] == ' ' || pbuf_[0] == '\t') {
        const unsigned room = sizeof(lbuf_) - 1 - llen_;
        const unsigned add = (unsigned)strlen(pbuf_ + 1);
        const unsigned n = add < room ? add : room;
        memcpy(lbuf_ + llen_, pbuf_ + 1, n);
        llen_ += n;
        lbuf_[llen_] = 0;
      } else {
        if (llen_) processLine(lbuf_);
        strcpy(lbuf_, pbuf_);   // both bounded to their buffers above
        llen_ = (unsigned)strlen(lbuf_);
      }
      plen_ = 0;
    }
  }

  // Flush the trailing line, then apply the RECURRENCE-ID edits. Overrides can
  // precede their master in the stream, which is why they wait until here.
  void end() {
    pbuf_[plen_] = 0;
    if (plen_ && (pbuf_[0] == ' ' || pbuf_[0] == '\t')) {
      feed("\n", 1);
    } else {
      if (llen_) processLine(lbuf_);
      llen_ = 0;
      if (plen_) processLine(pbuf_);
      plen_ = 0;
    }
    for (uint8_t i = 0; i < nOver_; i++) {
      removeAt(over_[i].orig);
      if (!over_[i].cancelled) offer(over_[i].ev);
    }
  }

  uint8_t        count() const { return count_; }
  const IcsOut&  get(uint8_t i) const { return out_[i < count_ ? i : 0]; }
  uint16_t       skippedRules() const { return skipped_; }

 private:
  // ---- current-event fields -------------------------------------------------
  struct Ev {
    bool    haveStart;
    int64_t sDay;            // civil day when local, epoch day when UTC
    int32_t sSod;            // seconds of day
    bool    sUtc, allDay;
    int64_t dur;             // seconds; from DTEND when present
    bool    haveEnd;
    char    title[ICS_TITLE_LEN];
    char    rrule[160];
    int64_t exdate[ICS_EXDATES];
    uint8_t nEx;
    int64_t recurId;         // 0 = not an override
    bool    cancelled;
  };
  Ev ev_;

  struct Override {
    int64_t orig;
    IcsOut  ev;
    bool    cancelled;
  };

  int64_t  winStart_, winEnd_;
  int      offMin_;
  IcsOut   out_[ICS_KEEP];
  uint8_t  count_;
  Override over_[ICS_OVERRIDES];
  uint8_t  nOver_;
  uint16_t skipped_;
  char     pbuf_[ICS_LINE_MAX];
  char     lbuf_[ICS_LINE_MAX];
  unsigned plen_, llen_;
  bool     inEvent_, inAlarm_;

  // ---- time helpers ---------------------------------------------------------
  // "20260810T143000Z" / "20260810T143000" / "20260810" in the fields ICS uses.
  static bool parseStamp(const char* v, int64_t* day, int32_t* sod, bool* utc,
                         bool* dateOnly) {
    auto num = [](const char* p, int n, int* out) {
      int r = 0;
      for (int i = 0; i < n; i++) {
        if (p[i] < '0' || p[i] > '9') return false;
        r = r * 10 + (p[i] - '0');
      }
      *out = r;
      return true;
    };
    int y, mo, d;
    if (!num(v, 4, &y) || !num(v + 4, 2, &mo) || !num(v + 6, 2, &d)) return false;
    if (mo < 1 || mo > 12 || d < 1 || d > 31) return false;
    *day = calDaysFromCivil(y, mo, d);
    *sod = 0;
    *utc = false;
    *dateOnly = true;
    if (v[8] == 'T') {
      int h, mi, s = 0;
      if (!num(v + 9, 2, &h) || !num(v + 11, 2, &mi)) return false;
      if (v[13] >= '0' && v[13] <= '9') num(v + 13, 2, &s);
      if (h > 23 || mi > 59 || s > 60) return false;
      *sod = h * 3600 + mi * 60 + (s > 59 ? 59 : s);
      *dateOnly = false;
      const char* z = v + 8;
      while (*z && *z != 'Z' && *z != 'z') z++;
      *utc = (*z != 0);
    }
    return true;
  }

  int64_t toEpoch(int64_t day, int32_t sod, bool utc) const {
    return day * 86400 + sod - (utc ? 0 : (int64_t)offMin_ * 60);
  }

  // ---- the keep-list --------------------------------------------------------
  void offer(const IcsOut& e) {
    if (e.end <= winStart_ || e.start >= winEnd_) return;
    // Insert sorted by start; the list holds the soonest ICS_KEEP.
    uint8_t pos = count_;
    while (pos && out_[pos - 1].start > e.start) pos--;
    if (pos >= ICS_KEEP) return;
    const uint8_t n = count_ < ICS_KEEP ? count_ : (uint8_t)(ICS_KEEP - 1);
    for (uint8_t i = n; i > pos; i--) out_[i] = out_[i - 1];
    out_[pos] = e;
    if (count_ < ICS_KEEP) count_++;
  }

  void removeAt(int64_t start) {
    for (uint8_t i = 0; i < count_; i++)
      if (out_[i].start == start) {
        for (uint8_t j = i; j + 1 < count_; j++) out_[j] = out_[j + 1];
        count_--;
        return;
      }
  }

  void emitOccurrence(int64_t day, int32_t sod) {
    IcsOut o;
    o.start = toEpoch(day, sod, ev_.sUtc);
    for (uint8_t i = 0; i < ev_.nEx; i++)
      if (ev_.exdate[i] == o.start) return;
    o.end = o.start + ev_.dur;
    o.allDay = ev_.allDay;
    memcpy(o.title, ev_.title, sizeof(o.title));
    offer(o);
  }

  // ---- RRULE ----------------------------------------------------------------
  void expand() {
    // Parsed with safe defaults; anything unrecognised marks the rule skipped
    // so the caller can say "some repeats were not understood".
    int  interval = 1;
    long count = -1;
    int64_t untilEpoch = INT64_MAX;
    uint8_t byday = 0;        // bitmask, bit 0 = Sunday
    int  nthDow = 0, nthN = 0;   // monthly nth-weekday (n can be -1)
    int  bymonthday = 0;
    char freq[10] = "";
    bool unsupported = false;

    const char* p = ev_.rrule;
    while (*p) {
      const char* eq = strchr(p, '=');
      if (!eq) break;
      const char* sc = strchr(eq, ';');
      const unsigned klen = (unsigned)(eq - p);
      char val[64];
      const unsigned vlen = (unsigned)((sc ? sc : eq + strlen(eq)) - (eq + 1));
      const unsigned vn = vlen < sizeof(val) - 1 ? vlen : sizeof(val) - 1;
      memcpy(val, eq + 1, vn);
      val[vn] = 0;

      if (!strncmp(p, "FREQ", klen) && klen == 4) {
        strncpy(freq, val, sizeof(freq) - 1);
      } else if (!strncmp(p, "INTERVAL", klen) && klen == 8) {
        interval = atoi(val);
        if (interval < 1) interval = 1;
      } else if (!strncmp(p, "COUNT", klen) && klen == 5) {
        count = atol(val);
      } else if (!strncmp(p, "UNTIL", klen) && klen == 5) {
        int64_t d; int32_t s; bool u, dateOnly;
        if (parseStamp(val, &d, &s, &u, &dateOnly))
          untilEpoch = toEpoch(d, dateOnly ? 86399 : s, u);
      } else if (!strncmp(p, "BYDAY", klen) && klen == 5) {
        static const char* dd = "SUMOTUWETHFRSA";
        const char* q = val;
        while (*q) {
          int n = 0, sign = 1;
          if (*q == '-') { sign = -1; q++; }
          else if (*q == '+') q++;
          while (*q >= '0' && *q <= '9') { n = n * 10 + (*q++ - '0'); }
          for (int i = 0; i < 7; i++)
            if (q[0] == dd[i * 2] && q[1] == dd[i * 2 + 1]) {
              byday |= (uint8_t)(1u << i);
              if (n) { nthN = sign * n; nthDow = i; }
              break;
            }
          while (*q && *q != ',') q++;
          if (*q == ',') q++;
        }
      } else if (!strncmp(p, "BYMONTHDAY", klen) && klen == 10) {
        bymonthday = atoi(val);
      } else if (!strncmp(p, "WKST", klen) && klen == 4) {
        // Only shifts which day a week starts on; irrelevant to the subset here.
      } else if (!strncmp(p, "BYMONTH", klen) && klen == 7) {
        // Yearly repeats already pin the month via DTSTART.
      } else {
        unsupported = true;      // BYSETPOS, BYWEEKNO, BYYEARDAY, ...
      }
      p = sc ? sc + 1 : eq + strlen(eq);
    }

    if (unsupported) { skipped_++; return; }

    long emitted = 0;
    long steps = 0;
    int sy, sm, sd;
    calCivilFromDays(ev_.sDay, &sy, &sm, &sd);

    auto due = [&](int64_t day) -> bool {   // shared count/until/window guard
      const int64_t t = toEpoch(day, ev_.sSod, ev_.sUtc);
      if (t > untilEpoch) return false;
      if (count >= 0 && emitted >= count) return false;
      emitted++;
      emitOccurrence(day, ev_.sSod);   // callers stop before winEnd_, so t < winEnd_
      (void)t;
      return true;
    };

    if (!strcmp(freq, "DAILY")) {
      for (int64_t day = ev_.sDay; steps++ < ICS_MAX_STEPS; day += interval) {
        if (toEpoch(day, ev_.sSod, ev_.sUtc) >= winEnd_) break;
        if (!due(day)) break;
      }
    } else if (!strcmp(freq, "WEEKLY")) {
      if (!byday) byday = (uint8_t)(1u << icsDow(ev_.sDay));
      // Walk day by day from the start; a day hits when its weekday is listed
      // and its week is on the interval. Weeks are counted Monday-based.
      const int64_t week0 = (ev_.sDay - ((icsDow(ev_.sDay) + 6) % 7)) / 7;
      for (int64_t day = ev_.sDay; steps++ < ICS_MAX_STEPS; day++) {
        if (toEpoch(day, ev_.sSod, ev_.sUtc) >= winEnd_) break;
        if (!(byday & (1u << icsDow(day)))) continue;
        const int64_t wk = (day - ((icsDow(day) + 6) % 7)) / 7;
        if ((wk - week0) % interval) continue;
        if (!due(day)) break;
      }
    } else if (!strcmp(freq, "MONTHLY")) {
      for (int k = 0; steps++ < ICS_MAX_STEPS; k += interval) {
        int y = sy + (sm - 1 + k) / 12;
        int m = (sm - 1 + k) % 12 + 1;
        int64_t day;
        if (nthN) {                          // "3rd Tuesday", "-1 = last Friday"
          const int64_t first = calDaysFromCivil(y, m, 1);
          const int64_t nextM = calDaysFromCivil(m == 12 ? y + 1 : y,
                                                 m == 12 ? 1 : m + 1, 1);
          if (nthN > 0) {
            day = first + ((nthDow - icsDow(first) + 7) % 7) + 7LL * (nthN - 1);
            if (day >= nextM) continue;      // no 5th Tuesday this month
          } else {
            const int64_t last = nextM - 1;
            day = last - ((icsDow(last) - nthDow + 7) % 7) + 7LL * (nthN + 1);
            if (day < first) continue;
          }
        } else {
          const int md = bymonthday ? bymonthday : sd;
          int cy, cm, cd;
          day = calDaysFromCivil(y, m, md);
          calCivilFromDays(day, &cy, &cm, &cd);
          if (cm != m) continue;             // the 31st of a 30-day month
        }
        if (toEpoch(day, ev_.sSod, ev_.sUtc) >= winEnd_) break;
        if (day < ev_.sDay) { emitted++; continue; }   // partial first month
        if (!due(day)) break;
      }
    } else if (!strcmp(freq, "YEARLY")) {
      for (int k = 0; steps++ < ICS_MAX_STEPS; k += interval) {
        const int64_t day = calDaysFromCivil(sy + k, sm, sd);
        int cy, cm, cd;
        calCivilFromDays(day, &cy, &cm, &cd);
        if (cm != sm) { emitted++; continue; }         // Feb 29 off-years
        if (toEpoch(day, ev_.sSod, ev_.sUtc) >= winEnd_) break;
        if (!due(day)) break;
      }
    } else {
      skipped_++;
    }
  }

  // ---- lines ----------------------------------------------------------------
  static void unescape(char* s) {
    char* w = s;
    for (const char* r = s; *r; r++) {
      if (*r == '\\' && r[1]) {
        r++;
        *w++ = (*r == 'n' || *r == 'N') ? ' ' : *r;
      } else {
        *w++ = *r;
      }
    }
    *w = 0;
  }

  void processLine(const char* line) {
    if (!strcmp(line, "BEGIN:VALARM")) { inAlarm_ = true; return; }
    if (!strcmp(line, "END:VALARM"))   { inAlarm_ = false; return; }
    if (inAlarm_) return;                // alarms carry SUMMARYs of their own

    if (!strcmp(line, "BEGIN:VEVENT")) {
      memset(&ev_, 0, sizeof(ev_));
      inEvent_ = true;
      return;
    }
    if (!strcmp(line, "END:VEVENT")) {
      if (inEvent_ && ev_.haveStart) finishEvent();
      inEvent_ = false;
      return;
    }
    if (!inEvent_) return;

    const char* colon = strchr(line, ':');
    if (!colon) return;
    const char* value = colon + 1;
    const char* semi = strchr(line, ';');
    const unsigned nameLen =
        (unsigned)(((semi && semi < colon) ? semi : colon) - line);
    const char* params = (semi && semi < colon) ? semi : colon;

    auto is = [&](const char* k) {
      return nameLen == strlen(k) && !strncmp(line, k, nameLen);
    };

    if (is("SUMMARY")) {
      icsCopy(ev_.title, value, sizeof(ev_.title));
      unescape(ev_.title);
      textFoldUtf8(ev_.title);   // curly quotes etc. -> ASCII the fonts have
    } else if (is("DTSTART")) {
      bool dateOnly;
      if (parseStamp(value, &ev_.sDay, &ev_.sSod, &ev_.sUtc, &dateOnly)) {
        ev_.haveStart = true;
        ev_.allDay = dateOnly || strstr(params, "VALUE=DATE") != nullptr;
      }
    } else if (is("DTEND")) {
      int64_t d; int32_t s; bool u, dateOnly;
      if (parseStamp(value, &d, &s, &u, &dateOnly)) {
        ev_.dur = toEpoch(d, s, u);      // finalised in finishEvent
        ev_.haveEnd = true;
      }
    } else if (is("RRULE")) {
      icsCopy(ev_.rrule, value, sizeof(ev_.rrule));
    } else if (is("EXDATE")) {
      // May carry a comma list; each entry cancels one occurrence.
      const char* q = value;
      while (*q && ev_.nEx < ICS_EXDATES) {
        int64_t d; int32_t s; bool u, dateOnly;
        if (parseStamp(q, &d, &s, &u, &dateOnly))
          ev_.exdate[ev_.nEx++] = toEpoch(d, s, u);
        while (*q && *q != ',') q++;
        if (*q == ',') q++;
      }
    } else if (is("RECURRENCE-ID")) {
      int64_t d; int32_t s; bool u, dateOnly;
      if (parseStamp(value, &d, &s, &u, &dateOnly))
        ev_.recurId = toEpoch(d, s, u);
    } else if (is("STATUS")) {
      ev_.cancelled = !strncmp(value, "CANCELLED", 9);
    }
  }

  void finishEvent() {
    const int64_t start = toEpoch(ev_.sDay, ev_.sSod, ev_.sUtc);
    ev_.dur = ev_.haveEnd ? ev_.dur - start : (ev_.allDay ? 86400 : 0);
    if (ev_.dur < 0) ev_.dur = 0;

    if (ev_.recurId) {                   // an edited (or cancelled) occurrence
      if (nOver_ < ICS_OVERRIDES) {
        Override& o = over_[nOver_++];
        o.orig = ev_.recurId;
        o.cancelled = ev_.cancelled;
        o.ev.start = start;
        o.ev.end = start + ev_.dur;
        o.ev.allDay = ev_.allDay;
        memcpy(o.ev.title, ev_.title, sizeof(o.ev.title));
      }
      return;
    }
    if (ev_.cancelled) return;
    if (ev_.rrule[0]) expand();
    else emitOccurrence(ev_.sDay, ev_.sSod);
  }
};
