// CalendarMode.h — the next obligation, on a cube on the desk.
//
// One glance answers one question: what is next, and how long until it. The
// biggest thing on screen is the next event's title and its countdown; a few
// upcoming events sit under it in small type. Everything else — providers,
// OAuth, recurrence — happens in CalendarClient and never on this screen.
//
// Recurring events never touch the device at all: both providers expand them
// server-side (singleEvents=true / calendarview), which is the difference
// between this file and an RRULE engine.
#pragma once
#include "config.h"
#if WITH_CALENDAR

#include "Mode.h"

class CalendarMode : public DisplayMode {
 public:
  const char* id() const override { return "calendar"; }
  uint8_t     modeConst() const override { return MODE_CALENDAR; }

  void begin(const Settings& s) override;
  void service(const Settings& s) override;
  void invalidate(const Settings& s) override;
  void wake(const Settings& s) override;

 private:
  void render(const Settings& s);

  bool     needFull_ = true;
  uint32_t lastDrawMs_ = 0;
  int      drawnNext_ = -2;      // calendarNextIdx() at the last paint
  bool     titleScrolls_ = false;
};

extern CalendarMode g_calendarMode;

#endif  // WITH_CALENDAR
