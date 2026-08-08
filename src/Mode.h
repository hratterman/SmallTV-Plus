// Mode.h — the display-mode interface.
//
// Each feature (ticker / usage / radar) is a self-contained DisplayMode: it owns
// its own data fetch, render, dirty-tracking and settings slice. main.cpp keeps a
// registry of the compiled-in modes and dispatches to whichever one matches the
// active settings.mode — it holds no per-feature state of its own.
#pragma once
#include <Arduino.h>
#include "Settings.h"

class DisplayMode {
 public:
  virtual ~DisplayMode() {}

  // Stable string id (also the settings.mode token, e.g. "stocks"/"usage"/"radar").
  virtual const char* id() const = 0;
  // The MODE_* constant this mode answers to (matched against settings.mode).
  virtual uint8_t modeConst() const = 0;

  virtual void begin(const Settings& s) {}          // one-time init at boot
  virtual void service(const Settings& s) {}        // every loop tick: fetch + render
  virtual void invalidate(const Settings& s) {}     // settings changed: re-init + repaint
  // Another mode drew on the screen (carousel switch): repaint from cached data,
  // do NOT refetch. Falls back to invalidate for modes without a light path.
  virtual void wake(const Settings& s) { invalidate(s); }

  // Long-press on the lid pad, while this mode is showing. Each mode decides
  // what its own gesture means; the default is to do nothing. Settings are
  // mutable so a mode can adjust its own slice, but changes are runtime-only —
  // nothing here writes flash, so a reboot returns to the saved configuration.
  virtual void onContextAction(Settings& s) {}
};
