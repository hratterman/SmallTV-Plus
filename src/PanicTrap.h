// PanicTrap — carry the panic's address, reason and backtrace across the
// reboot.
//
// The crash screen spent weeks showing "epc -" because the panic handler
// prints its diagnosis to a UART nobody is watching and then wipes the
// evidence with the reset. The Arduino core already interposes ESP-IDF's
// handler (-Wl,--wrap in its own link) and offers the result through
// set_arduino_panic_handler: reason string, faulting PC, and a walked
// backtrace. The trap stashes those in noinit DRAM - which survives the
// panic's reboot but not a power cycle - so the next boot can put a real
// address on the screen and the full call chain in /api/status.
// "Stack canary watchpoint triggered (weather)" names the guilty task
// outright; the addresses resolve to source lines with addr2line against
// this build's .elf.
#pragma once
#include <Arduino.h>

struct PanicTrapInfo {
  uint32_t pc;          // faulting instruction address
  char     reason[40];  // IDF's exception string
  uint8_t  btLen;       // valid entries in bt[]
  uint32_t bt[6];       // top of the walked backtrace, caller-first
};

#if defined(ESP8266)
// The 8266 already preserves its own epc1 through platformResetInfo.
static inline void panicTrapArm() {}
static inline bool panicTrapRead(PanicTrapInfo&) { return false; }
#else
void panicTrapArm();                  // register the handler; call early in setup
bool panicTrapRead(PanicTrapInfo& out);  // read-and-clear; false = no trap left
#endif
