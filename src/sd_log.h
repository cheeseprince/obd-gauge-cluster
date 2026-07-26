#pragma once
#include <cstdint>
#include "rtc.h"      // DateTime

enum class LogStatus : uint8_t { NoCard, Off, On, Err };

// --- Pure CSV row builders (host-tested). Iterate ACTIVE stats (displayed + hidden helpers) via READOUTS,
// StatId order. Buffers must be >= 512 bytes. Return the string length. ---
int buildHeaderRow(char* out, int n);   // "datetime,uptime_s,TRANS,OIL,...\n"
int buildDataRow(const DateTime& now, uint32_t uptimeS,
                 const float* v, const bool* valid, char* out, int n);  // blank cell where !valid
// Log filename (host-tested): "/logs/<stamp>.csv" when the RTC has actually been
// read (rtcValid), else "/logs/sessNNN.csv". The rtcValid gate matters: rtcNow
// defaults to a plausible date, so a y>=2000 check alone can NEVER fall back —
// every knob-absent/RTC-dead key-on would reuse one constant filename.
int buildLogPath(const DateTime& now, bool rtcValid, unsigned session, char* out, int n);

// --- Device SD I/O (core 0). Defined under ARDUINO; not linked on host. ---
void      sdBegin();       // init HSPI + SD.begin; status NoCard (no card) or Off (card ok)
LogStatus logStatus();     // current logger state (for the menu)
// Called each obd cycle: opens a file on link-up (enabled + card), writes a row every
// 1 s, closes on link-down. `now` = RTC wall-clock (filename + rows), trusted only when
// rtcValid; nowMs = millis(). NoCard/Err retry a remount every 30 s (late/reinserted
// card, transient write error) — errors are not latched for the whole drive.
void logTick(bool linked, bool enabled, const DateTime& now, bool rtcValid,
             uint32_t nowMs, const float* v, const bool* valid);
