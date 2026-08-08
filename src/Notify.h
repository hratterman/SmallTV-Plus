// Notify.h — a banner anything on the LAN can push onto the cube.
//
// POST /notify {"text":"...", "sec":8, "color":"#f7931a"} interrupts whatever
// mode is showing, holds the message for a few seconds, then hands the screen
// back. Deliberately trivial to call: one curl from a shell script, a Home
// Assistant automation, or a cron job on the Mini.
#pragma once
#include <Arduino.h>
#include "config.h"

// Show a banner. `sec` is clamped to something sane; colour is RGB565.
void notifyShow(const char* text, uint16_t sec, uint16_t color);

// Parse and apply a JSON body from the HTTP endpoint. Returns false on bad JSON
// or a missing/empty text field.
bool notifyApply(const String& json);

bool notifyActive();      // a banner is currently on screen
void notifyService();     // draws it once, then keeps time; call from the loop
void notifyDismiss();     // cut it short (the lid pad does this)
