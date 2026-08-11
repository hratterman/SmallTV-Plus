#include "config.h"
#if WITH_CALENDAR

#include "CalendarMode.h"
#include "CalendarClient.h"
#include "CalendarTime.h"
#include <Arduino_GFX_Library.h>
#include "Gfx.h"
#include "WorkMask.h"
#include "Clock.h"
#include <time.h>

CalendarMode g_calendarMode;

#define C_DIMTX  0x8410
#define C_FAINT  0x4208
#define C_ACCENT 0x34BF   // countdown blue
#define C_SOON   0xFD20   // orange once it is close
#define C_NOWC   0x07E0   // green while it is happening

// Layout. The header band, one hero event, then the list.
static const int HDR_Y      = 8;
static const int NEXT_LBL_Y = 40;
static const int TITLE_Y    = 56;
static const int WHEN_Y     = 84;
static const int LIST_Y     = 128;
static const int LIST_ROW   = 22;
static const int LIST_ROWS  = 4;
static const int FOOT_Y     = 226;
static const int TEXT_X     = 8;
static const int TEXT_W     = TFT_WIDTH - 2 * TEXT_X;
static_assert(TITLE_Y + 24 <= WHEN_Y, "the title band overlaps the when-line");
static_assert(WHEN_Y + 16 <= LIST_Y, "the when-line overlaps the list");
static_assert(LIST_Y + LIST_ROWS * LIST_ROW <= FOOT_Y, "the list runs into the footer");

static const GfxMarquee kTitleBand = {TEXT_X, TITLE_Y, TEXT_W, 2, C_WHITE, C_BLACK};

// "2:30 PM" / "14:30", prefixed with the day once it is not today. All-day
// events get the day alone — "all day 2:30" would be nonsense.
static void fmtWhen(int64_t startUtc, bool allDay, bool h12, char* out, size_t n) {
  const time_t now = time(nullptr);
  time_t st = (time_t)startUtc;
  struct tm lt, ln, lm;
  localtime_r(&st, &lt);
  localtime_r(&now, &ln);
  time_t tmr = now + 86400;
  localtime_r(&tmr, &lm);

  static const char* kDay[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  const bool today    = lt.tm_yday == ln.tm_yday && lt.tm_year == ln.tm_year;
  const bool tomorrow = lt.tm_yday == lm.tm_yday && lt.tm_year == lm.tm_year;

  if (allDay) {
    snprintf(out, n, "%s", today ? "all day" : (tomorrow ? "tomorrow" : kDay[lt.tm_wday]));
    return;
  }
  char day[8] = "";
  if (tomorrow)    snprintf(day, sizeof(day), "tmw ");
  else if (!today) snprintf(day, sizeof(day), "%s ", kDay[lt.tm_wday]);

  if (h12) {
    int h = lt.tm_hour % 12;
    if (!h) h = 12;
    snprintf(out, n, "%s%d:%02d %s", day, h, lt.tm_min, lt.tm_hour < 12 ? "AM" : "PM");
  } else {
    snprintf(out, n, "%s%d:%02d", day, lt.tm_hour, lt.tm_min);
  }
}

static void maskedTitle(const Settings& s, const CalEvent& e, char* out, size_t n) {
  strlcpy(out, e.title, n);
  // Meeting titles are as unfiltered as song titles; work mode treats them the
  // same way it treats Spotify's.
  if (s.work.enabled && s.work.hideExplicit)
    workMaskWords(out, s.work.blocklist.c_str());
}

void CalendarMode::begin(const Settings& s) {
  calendarInit(s);
  invalidate(s);
}

void CalendarMode::invalidate(const Settings& s) {
  calendarInit(s);        // config may have changed; the epoch bump is cheap
  needFull_ = true;
}

void CalendarMode::wake(const Settings& s) { (void)s; needFull_ = true; }

void CalendarMode::render(const Settings& s) {
  Arduino_GFX* gfx = gfxDev();
  if (!gfx) return;
  gfx->fillScreen(C_BLACK);
  titleScrolls_ = false;

  CalSnapshot snap;
  calendarSnapshot(snap);
  const int64_t now = (int64_t)time(nullptr);
  const int next = calSnapshotNext(snap, now);
  drawnNext_ = next;

  // Header: today, so the day names below have an anchor.
  {
    const time_t tnow = time(nullptr);
    struct tm lt;
    localtime_r(&tnow, &lt);
    static const char* kMon[12] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                   "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
    static const char* kDay[7] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    char hdr[28];
    snprintf(hdr, sizeof(hdr), "%s  %s %d", kDay[lt.tm_wday], kMon[lt.tm_mon], lt.tm_mday);
    gfxDrawCentered(hdr, HDR_Y, 1, C_DIMTX);
    gfx->drawFastHLine(TEXT_X, HDR_Y + 14, TEXT_W, C_FAINT);
  }

  // A link in progress owns the screen: the whole point of the device-code
  // flow is that the code is right here, not buried in a terminal.
  {
    CalLinkState link;
    calendarLinkState(link);
    if (link.phase == CAL_LINK_CODE) {
      gfxDrawCentered("go to", 60, 1, C_DIMTX);
      gfxDrawCentered(link.url, 76, 1, C_WHITE);
      gfxDrawCentered("and enter", 100, 1, C_DIMTX);
      gfxDrawCentered(link.code, 122, 3, C_ACCENT);
      gfxDrawCentered("waiting...", 170, 1, C_FAINT);
      return;
    }
    if (link.phase == CAL_LINK_FAILED && !s.calendar.refreshToken.length()) {
      gfxDrawCentered("linking failed", 96, 1, C_SOON);
      gfxDrawCentered(link.msg, 116, 1, C_DIMTX);
      return;
    }
  }

  // An imported calendar file fills the snapshot without anything being
  // "linked", so a good snapshot outranks the setup hints.
  const bool linked = s.calendar.provider == CAL_ICS
                          ? s.calendar.icsUrl.length() > 0
                          : s.calendar.refreshToken.length() > 0;
  if ((!s.calendar.enabled || !linked) && !snap.ok) {
    gfxDrawCentered("calendar not linked", 100, 1, C_DIMTX);
    gfxDrawCentered("easiest: paste the calendar's", 120, 1, C_FAINT);
    gfxDrawCentered("secret link in the web UI", 132, 1, C_FAINT);
    return;
  }
  if (!snap.ok) {
    gfxDrawCentered(snap.error[0] ? snap.error : "fetching...", 108, 1,
                    snap.error[0] ? C_SOON : C_DIMTX);
    return;
  }
  if (next < 0) {
    gfxDrawCentered("nothing ahead", 96, 2, C_WHITE);
    gfxDrawCentered("the next two days are clear", 122, 1, C_DIMTX);
    return;
  }

  const CalEvent& e = snap.events[next];

  gfx->setTextSize(1);
  gfx->setTextColor(C_FAINT);
  gfx->setCursor(TEXT_X, NEXT_LBL_Y);
  gfx->print(e.startUtc <= now ? "NOW" : "NEXT");

  char title[CAL_TITLE_LEN];
  maskedTitle(s, e, title, sizeof(title));
  // Sans typeface: a title that fits draws static in real type - the size-3
  // face if it fits, the size-2 face otherwise. Only a title too long for
  // both falls back to the pixel marquee (the scroller is pixel by design).
  titleScrolls_ = false;
  if (gfxTypeIsSans() && gfxLabelW(title, 3) <= TEXT_W) {
    gfxLabel(TEXT_X + (TEXT_W - gfxLabelW(title, 3)) / 2, TITLE_Y, title, 3, C_WHITE);
  } else if (gfxTypeIsSans() && gfxLabelW(title, 2) <= TEXT_W) {
    gfxLabel(TEXT_X + (TEXT_W - gfxLabelW(title, 2)) / 2, TITLE_Y + 4, title, 2, C_WHITE);
  } else {
    titleScrolls_ = gfxMarqueeDraw(kTitleBand, title, millis());
  }

  // When + countdown, coloured by urgency: green while it is on, orange in the
  // last ten minutes, calm blue with time in hand.
  {
    char when[20], cd[20], line[44];
    fmtWhen(e.startUtc, e.allDay, s.clock.mode12h, when, sizeof(when));
    calCountdown(e.startUtc - now, e.endUtc - now, cd, sizeof(cd));
    const int64_t dl = e.startUtc - now;
    const uint16_t col = dl <= 0 ? C_NOWC : (dl <= 600 ? C_SOON : C_ACCENT);
    snprintf(line, sizeof(line), "%s  .  %s", when, cd);
    gfxDrawCentered(line, WHEN_Y, 2, col);
  }

  // The list: what follows, small.
  int row = 0;
  for (uint8_t i = (uint8_t)next + 1; i < snap.count && row < LIST_ROWS; i++) {
    const CalEvent& le = snap.events[i];
    if (le.endUtc <= now) continue;
    const int y = LIST_Y + row * LIST_ROW;

    char when[20];
    fmtWhen(le.startUtc, le.allDay, s.clock.mode12h, when, sizeof(when));
    gfx->setTextSize(1);
    gfx->setTextColor(C_DIMTX);
    // Small metadata either way; centred against the taller sans title row.
    gfx->setCursor(TEXT_X, y + (gfxTypeIsSans() ? 4 : 0));
    gfx->print(when);

    char t[CAL_TITLE_LEN];
    maskedTitle(s, le, t, sizeof(t));
    const int tx = TEXT_X + 76, tw = TFT_WIDTH - tx - 4;
    if (gfxTypeIsSans()) {
      // Titles are the text of this screen: the size-2 sans face, trimmed to
      // its measured width rather than a fixed character count.
      size_t len = strlen(t);
      while (len && gfxLabelW(t, 2) > tw) t[--len] = 0;
      gfxLabel(tx, y, t, 2, C_WHITE);
    } else {
      // The pixel look keeps its original dense size-1 rows.
      t[26] = 0;
      gfx->setTextColor(C_WHITE);
      gfx->setCursor(tx, y);
      gfx->print(t);
    }
    row++;
  }
  if (row == 0) {
    gfx->setTextSize(1);
    gfx->setTextColor(C_FAINT);
    gfx->setCursor(TEXT_X, LIST_Y);
    gfx->print("nothing after this");
  }

  // Footer: the data's age, so a stale screen says so instead of lying calmly.
  {
    const uint32_t age = snap.ageMs / 60000UL;
    char f[28];
    if (age < 1) snprintf(f, sizeof(f), "just updated");
    else         snprintf(f, sizeof(f), "updated %lum ago", (unsigned long)age);
    gfxDrawCentered(f, FOOT_Y, 1, C_FAINT);
  }
}

void CalendarMode::service(const Settings& s) {
  const uint32_t nowMs = millis();
  bool repaint = needFull_;

  // Repaint when "next" changes (an event ended, or the first fetch landed),
  // and once a minute so the countdown ticks. While a link flow is running,
  // once a second — the code has to appear the moment Microsoft issues it.
  if (!repaint && nowMs - lastDrawMs_ >= 60000UL) repaint = true;
  {
    static uint8_t s_lastLink = 0;
    CalLinkState link;
    calendarLinkState(link);
    if (link.phase != s_lastLink) { s_lastLink = (uint8_t)link.phase; repaint = true; }
    else if (link.phase == CAL_LINK_CODE && nowMs - lastDrawMs_ >= 1000UL) repaint = true;
  }
  if (!repaint) {
    CalSnapshot snap;
    calendarSnapshot(snap);
    if (calSnapshotNext(snap, (int64_t)time(nullptr)) != drawnNext_ ||
        (snap.ok && drawnNext_ == -2))
      repaint = true;
  }

  if (repaint) {
    needFull_ = false;
    lastDrawMs_ = nowMs;
    render(s);
  } else if (titleScrolls_) {
    CalSnapshot snap;
    calendarSnapshot(snap);
    const int next = calSnapshotNext(snap, (int64_t)time(nullptr));
    if (next >= 0) {
      char title[CAL_TITLE_LEN];
      maskedTitle(s, snap.events[next], title, sizeof(title));
      titleScrolls_ = gfxMarqueeDraw(kTitleBand, title, millis());
    }
  }
}

#endif  // WITH_CALENDAR
