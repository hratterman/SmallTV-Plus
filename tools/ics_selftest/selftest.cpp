// Host-side checks for src/features/calendar/Ics.h — the secret-URL calendar
// feed, and above all its RRULE expansion.
//
// A recurrence engine fails quietly: the weekly standup simply is not there,
// or the cancelled meeting is, and the screen looks perfectly healthy either
// way. So the cases here are the calendar shapes people actually have — the
// MO/WE/FR standup, the biweekly one, rent on the 15th, the second-Monday
// meetup, the birthday — each pinned to exact expected timestamps around a
// fixed fake "now": Monday 2026-08-10, 12:00, UTC-7.
#include <cstdio>
#include <cstring>
#include <string>

#include "../../src/features/calendar/Ics.h"

static int failures = 0;
static void ck(bool cond, const char* what) {
  printf("  %-5s %s\n", cond ? "ok" : "FAIL", what);
  if (!cond) failures++;
}

static const int OFF = -7 * 60;                      // minutes east of UTC
static int64_t epochLocal(int y, int mo, int d, int h, int mi) {
  return calDaysFromCivil(y, mo, d) * 86400LL + h * 3600 + mi * 60 - OFF * 60;
}
static int64_t epochUtc(int y, int mo, int d, int h, int mi) {
  return calDaysFromCivil(y, mo, d) * 86400LL + h * 3600 + mi * 60;
}

static const int64_t NOW  = epochLocal(2026, 8, 10, 12, 0);   // Monday noon
static const int64_t WEND = NOW + 48 * 3600;

static void run(IcsParser& p, const std::string& ics, unsigned chunk = 4096,
                int64_t winEnd = WEND) {
  p.begin(NOW, winEnd, OFF);
  for (size_t i = 0; i < ics.size(); i += chunk)
    p.feed(ics.c_str() + i, (unsigned)std::min((size_t)chunk, ics.size() - i));
  p.end();
}

static bool has(const IcsParser& p, const char* title, int64_t start) {
  for (uint8_t i = 0; i < p.count(); i++)
    if (!strcmp(p.get(i).title, title) && p.get(i).start == start) return true;
  return false;
}

static std::string ev(const char* body) {
  return std::string("BEGIN:VEVENT\r\n") + body + "END:VEVENT\r\n";
}

int main() {
  printf("--- civil round trip -----------------------------------------\n");
  {
    bool ok = true;
    for (int64_t day = 0; day < 60000; day += 13) {
      int y, m, d;
      calCivilFromDays(day, &y, &m, &d);
      if (calDaysFromCivil(y, m, d) != day) ok = false;
    }
    ck(ok, "calCivilFromDays inverts calDaysFromCivil across 160 years");
    ck(icsDow(calDaysFromCivil(2026, 8, 10)) == 1, "2026-08-10 is a Monday");
  }

  printf("\n--- plain events ---------------------------------------------\n");
  {
    IcsParser p;
    run(p, ev("SUMMARY:Dentist\r\nDTSTART:20260811T160000Z\r\nDTEND:20260811T170000Z\r\n"));
    ck(has(p, "Dentist", epochUtc(2026, 8, 11, 16, 0)), "a UTC event lands where it says");
    ck(p.get(0).end - p.get(0).start == 3600, "and keeps its duration");
  }
  {
    IcsParser p;
    run(p, ev("SUMMARY:Coffee\r\nDTSTART;TZID=America/Denver:20260811T090000\r\n"
              "DTEND;TZID=America/Denver:20260811T093000\r\n"));
    ck(has(p, "Coffee", epochLocal(2026, 8, 11, 9, 0)),
       "a TZID event is read as cube-local time");
  }
  {
    IcsParser p;
    run(p, ev("SUMMARY:Conference\r\nDTSTART;VALUE=DATE:20260811\r\nDTEND;VALUE=DATE:20260812\r\n"));
    ck(p.count() == 1 && p.get(0).allDay, "an all-day event is flagged all-day");
    ck(p.get(0).start == epochLocal(2026, 8, 11, 0, 0), "and starts at local midnight");
  }
  {
    IcsParser p;   // started yesterday, ends tomorrow: it is happening now
    run(p, ev("SUMMARY:Offsite\r\nDTSTART:20260809T190000Z\r\nDTEND:20260812T190000Z\r\n"));
    ck(p.count() == 1, "an event already in progress is kept");
  }
  {
    IcsParser p;
    run(p, ev("SUMMARY:Old\r\nDTSTART:20200810T190000Z\r\nDTEND:20200810T200000Z\r\n") +
           ev("SUMMARY:Far\r\nDTSTART:20270810T190000Z\r\nDTEND:20270810T200000Z\r\n"));
    ck(p.count() == 0, "the past and the far future are both dropped");
  }

  printf("\n--- the standup (weekly BYDAY) -------------------------------\n");
  const std::string standup =
      ev("SUMMARY:Standup\r\nDTSTART;TZID=America/Denver:20190107T143000\r\n"
         "DTEND;TZID=America/Denver:20190107T144500\r\n"
         "RRULE:FREQ=WEEKLY;BYDAY=MO,WE,FR\r\n");
  // The standup is at 14:30 and the 48 h window ends Wednesday noon, so these
  // runs widen to 72 h — the point is the weekday pattern, not the window edge
  // (which the plain-event section already pins).
  const int64_t W72 = NOW + 72 * 3600;
  {
    IcsParser p;
    run(p, standup, 4096, W72);
    ck(has(p, "Standup", epochLocal(2026, 8, 10, 14, 30)),
       "a 2019 MO/WE/FR rule produces Monday's occurrence");
    ck(has(p, "Standup", epochLocal(2026, 8, 12, 14, 30)), "and Wednesday's");
    ck(!has(p, "Standup", epochLocal(2026, 8, 11, 14, 30)), "but not Tuesday");
  }
  {
    IcsParser p;   // EXDATE kills exactly one occurrence
    run(p, ev("SUMMARY:Standup\r\nDTSTART;TZID=America/Denver:20190107T143000\r\n"
              "DTEND;TZID=America/Denver:20190107T144500\r\n"
              "RRULE:FREQ=WEEKLY;BYDAY=MO,WE,FR\r\n"
              "EXDATE;TZID=America/Denver:20260812T143000\r\n"),
        4096, W72);
    ck(has(p, "Standup", epochLocal(2026, 8, 10, 14, 30)) &&
           !has(p, "Standup", epochLocal(2026, 8, 12, 14, 30)),
       "EXDATE removes Wednesday and only Wednesday");
  }
  {
    // A moved occurrence, with the override arriving BEFORE its master —
    // stream order must not matter.
    IcsParser p;
    run(p, ev("SUMMARY:Standup (moved)\r\n"
              "RECURRENCE-ID;TZID=America/Denver:20260810T143000\r\n"
              "DTSTART;TZID=America/Denver:20260810T153000\r\n"
              "DTEND;TZID=America/Denver:20260810T154500\r\n") +
           standup, 4096, W72);
    ck(!has(p, "Standup", epochLocal(2026, 8, 10, 14, 30)),
       "the original Monday slot is gone");
    ck(has(p, "Standup (moved)", epochLocal(2026, 8, 10, 15, 30)),
       "and the moved one is there instead");
    ck(has(p, "Standup", epochLocal(2026, 8, 12, 14, 30)), "Wednesday is untouched");
  }
  {
    IcsParser p;   // a cancelled occurrence just disappears
    run(p, standup +
           ev("SUMMARY:Standup\r\nSTATUS:CANCELLED\r\n"
              "RECURRENCE-ID;TZID=America/Denver:20260812T143000\r\n"
              "DTSTART;TZID=America/Denver:20260812T143000\r\n"),
        4096, W72);
    ck(has(p, "Standup", epochLocal(2026, 8, 10, 14, 30)) &&
           !has(p, "Standup", epochLocal(2026, 8, 12, 14, 30)),
       "a cancelled Wednesday stays cancelled");
  }

  printf("\n--- intervals, COUNT, UNTIL ----------------------------------\n");
  {
    IcsParser p;   // biweekly from 2026-07-27 (2 weeks before): due this week
    run(p, ev("SUMMARY:Biweekly\r\nDTSTART;TZID=X:20260727T130000\r\n"
              "DTEND;TZID=X:20260727T140000\r\nRRULE:FREQ=WEEKLY;INTERVAL=2;BYDAY=MO\r\n"));
    ck(has(p, "Biweekly", epochLocal(2026, 8, 10, 13, 0)),
       "a biweekly Monday two weeks back lands today");
  }
  {
    IcsParser p;   // biweekly from last Monday: off-week now
    run(p, ev("SUMMARY:Biweekly\r\nDTSTART;TZID=X:20260803T100000\r\n"
              "DTEND;TZID=X:20260803T140000\r\nRRULE:FREQ=WEEKLY;INTERVAL=2;BYDAY=MO\r\n"));
    ck(p.count() == 0, "and from one week back it is the off week");
  }
  {
    IcsParser p;   // ten dailies starting 2026-08-05: the last is Aug 14 — active
    run(p, ev("SUMMARY:Course\r\nDTSTART;TZID=X:20260805T180000\r\n"
              "DTEND;TZID=X:20260805T190000\r\nRRULE:FREQ=DAILY;COUNT=10\r\n"));
    ck(has(p, "Course", epochLocal(2026, 8, 11, 18, 0)), "a running COUNT series shows");
  }
  {
    IcsParser p;   // three dailies in 2020: exhausted long ago
    run(p, ev("SUMMARY:Course\r\nDTSTART;TZID=X:20200805T180000\r\n"
              "DTEND;TZID=X:20200805T190000\r\nRRULE:FREQ=DAILY;COUNT=3\r\n"));
    ck(p.count() == 0, "an exhausted COUNT series does not");
  }
  {
    IcsParser p;   // UNTIL before the window
    run(p, ev("SUMMARY:Gym\r\nDTSTART;TZID=X:20250101T070000\r\n"
              "DTEND;TZID=X:20250101T080000\r\n"
              "RRULE:FREQ=DAILY;UNTIL=20260701T000000Z\r\n"));
    ck(p.count() == 0, "a series UNTIL-ed before the window is silent");
  }

  printf("\n--- monthly and yearly ---------------------------------------\n");
  {
    IcsParser p;   // rent on the 11th
    run(p, ev("SUMMARY:Rent\r\nDTSTART;TZID=X:20240111T090000\r\n"
              "DTEND;TZID=X:20240111T091500\r\nRRULE:FREQ=MONTHLY;BYMONTHDAY=11\r\n"));
    ck(has(p, "Rent", epochLocal(2026, 8, 11, 9, 0)), "monthly-by-monthday hits the 11th");
  }
  {
    IcsParser p;   // 2nd Monday: Aug 2026's is the 10th
    run(p, ev("SUMMARY:Meetup\r\nDTSTART;TZID=X:20250210T190000\r\n"
              "DTEND;TZID=X:20250210T210000\r\nRRULE:FREQ=MONTHLY;BYDAY=2MO\r\n"));
    ck(has(p, "Meetup", epochLocal(2026, 8, 10, 19, 0)),
       "the second Monday of the month is found");
  }
  {
    IcsParser p;   // last Friday of Aug 2026 is the 28th — outside the window
    run(p, ev("SUMMARY:Retro\r\nDTSTART;TZID=X:20250131T150000\r\n"
              "DTEND;TZID=X:20250131T160000\r\nRRULE:FREQ=MONTHLY;BYDAY=-1FR\r\n"));
    ck(p.count() == 0, "the last Friday falls outside this window");
  }
  {
    IcsParser p;   // birthday, all-day, yearly since 1990
    run(p, ev("SUMMARY:Sam's birthday\r\nDTSTART;VALUE=DATE:19900811\r\n"
              "RRULE:FREQ=YEARLY\r\n"));
    ck(has(p, "Sam's birthday", epochLocal(2026, 8, 11, 0, 0)),
       "a yearly all-day birthday shows on its day");
    ck(p.count() == 1 && p.get(0).allDay, "still flagged all-day");
  }
  {
    IcsParser p;   // Feb 29 birthday, window not near Feb: nothing, and no phantom Mar 1
    run(p, ev("SUMMARY:Leap\r\nDTSTART;VALUE=DATE:20240229\r\nRRULE:FREQ=YEARLY\r\n"));
    ck(p.count() == 0, "a Feb-29 series emits nothing in a non-leap August");
  }

  printf("\n--- honesty about the unsupported ----------------------------\n");
  {
    IcsParser p;
    run(p, ev("SUMMARY:Weird\r\nDTSTART;TZID=X:20260810T130000\r\n"
              "DTEND;TZID=X:20260810T140000\r\n"
              "RRULE:FREQ=MONTHLY;BYDAY=MO,TU,WE,TH,FR;BYSETPOS=-1\r\n"));
    ck(p.count() == 0 && p.skippedRules() == 1,
       "a BYSETPOS rule is skipped and counted, not mangled");
  }

  printf("\n--- the wire format ------------------------------------------\n");
  {
    IcsParser p;   // folded SUMMARY across three physical lines
    run(p, ev("SUMMARY:This meeting title is long enough th\r\n at it was folded acr\r\n oss lines\r\n"
              "DTSTART:20260811T160000Z\r\nDTEND:20260811T163000Z\r\n"));
    char want[ICS_TITLE_LEN];
    snprintf(want, sizeof(want), "%s",
             "This meeting title is long enough that it was folded across lines");
    ck(p.count() == 1 && !strcmp(p.get(0).title, want),
       "a folded title is reassembled (and clipped to its buffer)");
  }
  {
    IcsParser p;
    run(p, ev("SUMMARY:Lunch\\, maybe\\nor not\r\nDTSTART:20260811T180000Z\r\nDTEND:20260811T190000Z\r\n"));
    ck(p.count() == 1 && !strcmp(p.get(0).title, "Lunch, maybe or not"),
       "ICS escapes are unescaped");
  }
  {
    IcsParser p;   // a VALARM's SUMMARY must not replace the event's
    run(p, ev("SUMMARY:Real title\r\nDTSTART:20260811T160000Z\r\nDTEND:20260811T170000Z\r\n"
              "BEGIN:VALARM\r\nACTION:EMAIL\r\nSUMMARY:Alarm text\r\nEND:VALARM\r\n"));
    ck(p.count() == 1 && !strcmp(p.get(0).title, "Real title"),
       "an alarm block's SUMMARY is ignored");
  }
  {
    // Byte-at-a-time streaming must equal one-shot parsing.
    IcsParser a, b;
    const std::string feed = standup +
        ev("SUMMARY:Dentist\r\nDTSTART:20260811T160000Z\r\nDTEND:20260811T170000Z\r\n");
    run(a, feed);
    run(b, feed, 1);
    bool same = a.count() == b.count();
    for (uint8_t i = 0; same && i < a.count(); i++)
      same = a.get(i).start == b.get(i).start &&
             !strcmp(a.get(i).title, b.get(i).title);
    ck(same, "one byte at a time parses identically to one shot");
  }
  {
    // More events than the keep-list holds: the soonest survive, sorted.
    std::string many;
    for (int d = 22; d >= 11; d--) {   // Aug 11..22, added farthest-first
      char b[160];
      snprintf(b, sizeof(b),
               "SUMMARY:E%d\r\nDTSTART:202608%02dT150000Z\r\nDTEND:202608%02dT160000Z\r\n",
               d, d, d);
      many += ev(b);
    }
    IcsParser p;
    p.begin(NOW, NOW + 30 * 86400, OFF);   // wide window on purpose
    p.feed(many.c_str(), (unsigned)many.size());
    p.end();
    ck(p.count() == ICS_KEEP, "the keep-list fills to its cap");
    bool sorted = true, soonest = !strcmp(p.get(0).title, "E11");
    for (uint8_t i = 1; i < p.count(); i++)
      if (p.get(i).start < p.get(i - 1).start) sorted = false;
    ck(sorted && soonest, "and holds the soonest events in order");
  }

  printf("\n--- UTF-8 fold (TextFold.h) ----------------------------------\n");
  {
    auto fold = [](const char* in, const char* want, const char* what) {
      char buf[96];
      strncpy(buf, in, sizeof(buf) - 1);
      buf[sizeof(buf) - 1] = 0;
      textFoldUtf8(buf);
      char msg[128];
      snprintf(msg, sizeof(msg), "%s -> \"%s\"", what, buf);
      ck(!strcmp(buf, want), msg);
    };
    fold("Doctor\xE2\x80\x99s Appointment", "Doctor's Appointment",
         "curly apostrophe becomes '");
    fold("\xE2\x80\x9Cscare quotes\xE2\x80\x9D", "\"scare quotes\"",
         "curly doubles become \"");
    fold("Caf\xC3\xA9 \xE2\x80\x93 review", "Cafe - review",
         "accent stripped, en dash to hyphen");
    fold("\xC5\x81ukasz \xC5\xBDluti\xC4\x87", "Lukasz Zlutic",
         "Latin Extended-A folds to base letters");
    fold("wait\xE2\x80\xA6", "wait...", "ellipsis expands to dots");
    fold("hi \xF0\x9F\x98\x80!", "hi !", "emoji is dropped, not mangled");
    fold("a\xFF" "b\x80" "c", "abc", "stray bytes are dropped");
    fold("plain ASCII stays put", "plain ASCII stays put", "ASCII untouched");
  }
  {
    // End to end: the parser folds SUMMARY as it stores it.
    IcsParser p;
    p.begin(NOW, NOW + 7 * 86400, OFF);
    const char* ics =
        "BEGIN:VEVENT\r\n"
        "UID:fold1\r\n"
        "SUMMARY:Doctor\xE2\x80\x99s Appointment\r\n"
        "DTSTART:20260812T150000Z\r\n"
        "DTEND:20260812T160000Z\r\n"
        "END:VEVENT\r\n";
    p.feed(ics, (unsigned)strlen(ics));
    p.end();
    ck(p.count() == 1 && !strcmp(p.get(0).title, "Doctor's Appointment"),
       "an ICS SUMMARY with a curly apostrophe lands folded");
  }

  printf("\n-------------------------------------------------------------\n");
  if (failures) { printf("%d check(s) FAILED\n", failures); return 1; }
  printf("all checks passed\n");
  return 0;
}
