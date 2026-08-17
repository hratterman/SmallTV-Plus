#include "PanicTrap.h"
#if !defined(ESP8266)

#include "esp32-hal.h"   // arduino_panic_info_t, set_arduino_panic_handler

// Noinit: random garbage at power-on (the magic gate handles that), intact
// across the reboot a panic triggers.
__NOINIT_ATTR static uint32_t s_magic;
__NOINIT_ATTR static PanicTrapInfo s_trap;

#define TRAP_MAGIC 0xB0070FF5UL

// Panic context: no heap, no locks, no printf - plain stores only. The
// reason string lives in flash rodata, exactly where the core's own handler
// reads it from, so copying it here is as safe as that handler itself.
static void trapHandler(arduino_panic_info_t* info, void*) {
  s_trap.pc = (uint32_t)(uintptr_t)info->pc;
  const char* r = info->reason ? info->reason : "";
  size_t i = 0;
  for (; i < sizeof(s_trap.reason) - 1 && r[i]; i++) s_trap.reason[i] = r[i];
  s_trap.reason[i] = 0;
  uint8_t n = 0;
  for (; n < 6 && n < info->backtrace_len; n++) s_trap.bt[n] = info->backtrace[n];
  s_trap.btLen = n;
  s_magic = TRAP_MAGIC;
}

void panicTrapArm() { set_arduino_panic_handler(trapHandler, nullptr); }

bool panicTrapRead(PanicTrapInfo& out) {
  if (s_magic != TRAP_MAGIC) return false;
  s_magic = 0;
  out = s_trap;
  if (out.btLen > 6) out.btLen = 6;   // noinit garbage belt-and-braces
  out.reason[sizeof(out.reason) - 1] = 0;
  return true;
}

#endif  // !ESP8266
