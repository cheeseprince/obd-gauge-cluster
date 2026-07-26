#pragma once
#include <cstdint>

// Wall-clock date/time. y = full year (e.g. 2026).
struct DateTime { int y; uint8_t mon, d, h, min, s; };

// --- Pure helpers (host-tested) ---
uint8_t decToBcd(uint8_t v);
uint8_t bcdToDec(uint8_t v);
int  daysInMonth(int y, uint8_t mon);       // leap-year aware
bool dtValid(const DateTime& t);            // range sanity (year 2000-2099)
void adjustField(DateTime& t, int field, int delta);  // 0=y 1=mon 2=d 3=h 4=min; wraps + clamps day
void formatDateTime(const DateTime& t, char* out, int n);  // "2026-07-01 14:30:12"
void formatStamp(const DateTime& t, char* out, int n);     // "20260701_143012" (filenames)
// Decode the 7 raw PCF8563 registers (0x02..0x08: sec,min,hour,day,weekday,month,year)
// into out. False if the VL flag (seconds bit 7) is set — clock integrity lost (coin
// cell dead/removed), so the BCD content is garbage even when it range-checks — or if
// the decoded fields fail dtValid().
bool rtcDecodeRegs(const uint8_t regs[7], DateTime& out);

// --- Device I2C (PCF8563 @ 0x51), core-0 only. Defined under ARDUINO; not linked on host. ---
bool rtcRead(DateTime& out);        // read the RTC registers -> out; false on I2C error/invalid
bool rtcWrite(const DateTime& t);   // write t, then read back and confirm it matches
