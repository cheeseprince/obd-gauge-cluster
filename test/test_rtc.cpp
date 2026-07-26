#include <cstdio>
#include <cstring>
#include "rtc.h"

static int failures = 0;
static void check(bool c, const char* m){ if(!c){ printf("FAIL: %s\n", m); failures++; } }

int main() {
  // BCD round-trip.
  for (int v = 0; v <= 99; v++) check(bcdToDec(decToBcd((uint8_t)v)) == v, "bcd round-trip");
  check(decToBcd(59) == 0x59, "decToBcd 59 -> 0x59");
  check(bcdToDec(0x30) == 30, "bcdToDec 0x30 -> 30");

  // daysInMonth incl. leap years.
  check(daysInMonth(2026, 2) == 28, "2026 Feb = 28");
  check(daysInMonth(2024, 2) == 29, "2024 Feb = 29 (leap)");
  check(daysInMonth(2000, 2) == 29, "2000 Feb = 29 (div-400)");
  check(daysInMonth(2026, 4) == 30, "2026 Apr = 30");
  check(daysInMonth(2026, 1) == 31, "2026 Jan = 31");

  // dtValid.
  check( dtValid(DateTime{2026,7,1,14,30,12}), "valid dt");
  check(!dtValid(DateTime{2026,13,1,0,0,0}),   "month 13 invalid");
  check(!dtValid(DateTime{2026,2,30,0,0,0}),   "Feb 30 invalid");
  check(!dtValid(DateTime{1999,1,1,0,0,0}),    "year 1999 invalid");
  check(!dtValid(DateTime{2026,1,1,24,0,0}),   "hour 24 invalid");

  // adjustField: wrap.
  { DateTime t{2026,7,1,14,59,0}; adjustField(t,4,+1); check(t.min==0,  "min 59+1 -> 0 wrap"); }
  { DateTime t{2026,7,1,23,0,0};  adjustField(t,3,+1); check(t.h==0,    "hour 23+1 -> 0 wrap"); }
  { DateTime t{2026,12,1,0,0,0};  adjustField(t,1,+1); check(t.mon==1,  "month 12+1 -> 1 wrap"); }
  { DateTime t{2099,1,1,0,0,0};   adjustField(t,0,+1); check(t.y==2000, "year 2099+1 -> 2000 wrap"); }
  { DateTime t{2026,1,1,0,0,0};   adjustField(t,3,-1); check(t.h==23,   "hour 0-1 -> 23 wrap"); }

  // adjustField: day clamps when the month shrinks (Jan 31 -> Feb).
  { DateTime t{2026,1,31,0,0,0};  adjustField(t,1,+1); check(t.mon==2 && t.d==28, "Jan31 -> Feb clamps day to 28"); }
  { DateTime t{2024,1,31,0,0,0};  adjustField(t,1,+1); check(t.mon==2 && t.d==29, "leap Jan31 -> Feb 29"); }

  // Formatting.
  char b[32];
  formatDateTime(DateTime{2026,7,1,14,30,12}, b, sizeof b);
  check(std::strcmp(b, "2026-07-01 14:30:12") == 0, "formatDateTime");
  formatStamp(DateTime{2026,7,1,14,30,12}, b, sizeof b);
  check(std::strcmp(b, "20260701_143012") == 0, "formatStamp");

  // rtcDecodeRegs: raw PCF8563 registers (sec,min,hour,day,weekday,month,year, BCD).
  {
    DateTime o{};
    const uint8_t good[7] = {0x12, 0x30, 0x14, 0x01, 0x03, 0x07, 0x26};   // 2026-07-01 14:30:12
    check(rtcDecodeRegs(good, o), "decode good regs");
    check(o.y==2026 && o.mon==7 && o.d==1 && o.h==14 && o.min==30 && o.s==12, "decoded fields");

    // VL flag (seconds bit 7) = clock integrity lost -> must be rejected even
    // though the remaining BCD is plausible (dead coin cell -> garbage time).
    const uint8_t vl[7] = {0x92, 0x30, 0x14, 0x01, 0x03, 0x07, 0x26};     // 0x80 | 0x12
    check(!rtcDecodeRegs(vl, o), "VL flag set -> rejected");

    // Century bit on month must be masked, not corrupt the month.
    const uint8_t cent[7] = {0x12, 0x30, 0x14, 0x01, 0x03, 0x87, 0x26};   // 0x80 | 0x07
    check(rtcDecodeRegs(cent, o) && o.mon == 7, "century bit masked");

    // Garbage BCD that survives masking still fails dtValid.
    const uint8_t junk[7] = {0x12, 0x30, 0x14, 0x00, 0x03, 0x07, 0x26};   // day 0
    check(!rtcDecodeRegs(junk, o), "day 0 -> rejected");
  }

  printf(failures ? "\n%d FAILED\n" : "\nALL PASS\n", failures);
  return failures ? 1 : 0;
}
