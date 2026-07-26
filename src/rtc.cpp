#include "rtc.h"
#include <cstdio>

uint8_t decToBcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
uint8_t bcdToDec(uint8_t v) { return (uint8_t)(((v >> 4) * 10) + (v & 0x0F)); }

int daysInMonth(int y, uint8_t mon) {
  static const uint8_t dm[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
  if (mon < 1 || mon > 12) return 31;
  if (mon == 2) { bool leap = (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0); return leap ? 29 : 28; }
  return dm[mon - 1];
}

bool dtValid(const DateTime& t) {
  return t.y >= 2000 && t.y <= 2099 && t.mon >= 1 && t.mon <= 12 &&
         t.d >= 1 && t.d <= daysInMonth(t.y, t.mon) &&
         t.h <= 23 && t.min <= 59 && t.s <= 59;
}

// Wrap v into [lo, hi] inclusive.
static int wrapRange(int v, int lo, int hi) {
  int n = hi - lo + 1;
  while (v < lo) v += n;
  while (v > hi) v -= n;
  return v;
}

void adjustField(DateTime& t, int field, int delta) {
  switch (field) {
    case 0: t.y   = 2000 + wrapRange((t.y - 2000) + delta, 0, 99); break;
    case 1: t.mon = (uint8_t)wrapRange((int)t.mon + delta, 1, 12); break;
    case 2: t.d   = (uint8_t)wrapRange((int)t.d + delta, 1, daysInMonth(t.y, t.mon)); break;
    case 3: t.h   = (uint8_t)wrapRange((int)t.h + delta, 0, 23); break;
    case 4: t.min = (uint8_t)wrapRange((int)t.min + delta, 0, 59); break;
  }
  int dim = daysInMonth(t.y, t.mon);   // month/year change may leave day out of range
  if (t.d > dim) t.d = (uint8_t)dim;
  if (t.d < 1)   t.d = 1;
}

void formatDateTime(const DateTime& t, char* out, int n) {
  snprintf(out, n, "%04d-%02u-%02u %02u:%02u:%02u",
           t.y, (unsigned)t.mon, (unsigned)t.d, (unsigned)t.h, (unsigned)t.min, (unsigned)t.s);
}

void formatStamp(const DateTime& t, char* out, int n) {
  snprintf(out, n, "%04d%02u%02u_%02u%02u%02u",
           t.y, (unsigned)t.mon, (unsigned)t.d, (unsigned)t.h, (unsigned)t.min, (unsigned)t.s);
}

bool rtcDecodeRegs(const uint8_t regs[7], DateTime& o) {
  if (regs[0] & 0x80) return false;   // PCF8563 VL flag: oscillator stopped since last
                                      // set (battery dead) -> time is NOT trustworthy,
                                      // even if the BCD happens to range-check
  o.s   = bcdToDec(regs[0] & 0x7F);
  o.min = bcdToDec(regs[1] & 0x7F);
  o.h   = bcdToDec(regs[2] & 0x3F);
  o.d   = bcdToDec(regs[3] & 0x3F);
  // regs[4] = weekday — unused
  o.mon = bcdToDec(regs[5] & 0x1F);   // mask century bit
  o.y   = 2000 + bcdToDec(regs[6]);
  return dtValid(o);
}

#ifdef ARDUINO
#include <Wire.h>
#define RTC_ADDR 0x51
#define RTC_BASE 0x02   // PCF8563 seconds register (VL flag in bit7)

bool rtcRead(DateTime& o) {
  Wire.beginTransmission(RTC_ADDR);
  Wire.write((uint8_t)RTC_BASE);
  if (Wire.endTransmission() != 0) return false;
  if (Wire.requestFrom((uint8_t)RTC_ADDR, (uint8_t)7) != 7) return false;
  uint8_t regs[7];
  for (int i = 0; i < 7; i++) regs[i] = Wire.read();
  return rtcDecodeRegs(regs, o);   // rejects VL (dead coin cell) + out-of-range BCD
}

bool rtcWrite(const DateTime& t) {
  if (!dtValid(t)) return false;
  Wire.beginTransmission(RTC_ADDR);
  Wire.write((uint8_t)RTC_BASE);
  Wire.write(decToBcd(t.s));
  Wire.write(decToBcd(t.min));
  Wire.write(decToBcd(t.h));
  Wire.write(decToBcd(t.d));
  Wire.write((uint8_t)0);                    // weekday — unused
  Wire.write(decToBcd(t.mon));               // century bit 0 = 2000s
  Wire.write(decToBcd((uint8_t)(t.y - 2000)));
  if (Wire.endTransmission() != 0) return false;
  DateTime rb;                               // readback verify (wrong-map / bad-write backstop)
  if (!rtcRead(rb)) return false;
  return rb.y == t.y && rb.mon == t.mon && rb.d == t.d && rb.h == t.h && rb.min == t.min;
}
#endif
