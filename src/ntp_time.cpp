#include "ntp_time.h"

// Pure: apply the standard UTC offset, then the US-DST rule. DST is decided from
// the standard-offset local hour, so the Nov fall-back repeated hour (local
// 01:00-01:59) can read one hour fast until the next sync — an accepted ~1h/year
// edge for an on-demand clock set. Spring-forward is unaffected and no
// non-existent local time is ever emitted.
DateTime utcToLocal(time_t utc, const GeoLocation& geo) {
  time_t local = utc + (time_t)geo.tzStd * 3600;
  struct tm tmv{};
  gmtime_r(&local, &tmv);
  if (isUsDst(tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour)) {
    local += 3600;
    gmtime_r(&local, &tmv);
  }
  DateTime d;
  d.y   = tmv.tm_year + 1900;
  d.mon = (uint8_t)(tmv.tm_mon + 1);
  d.d   = (uint8_t)tmv.tm_mday;
  d.h   = (uint8_t)tmv.tm_hour;
  d.min = (uint8_t)tmv.tm_min;
  d.s   = (uint8_t)tmv.tm_sec;
  return d;
}

#ifdef ARDUINO
#include <Arduino.h>
#include <WiFi.h>

bool ntpSyncRtc(const GeoLocation& geo, void (*pump)(const char* status)) {
  // Terminal status lines get a brief hold (delay 1500) so they are readable —
  // otherwise the OTA flow immediately overwrites them with "checking...".
  if (!geoValid(geo)) { if (pump) { pump("Update: set location for time"); delay(1500); } return false; }
  if (pump) pump("Update: syncing time...");
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");   // UTC; two servers
  time_t now = 0;
  for (int i = 0; i < 50; i++) {          // up to ~5 s for SNTP to answer
    now = time(nullptr);
    if (now > 1600000000) break;          // > 2020-09 → the clock is real
    delay(100);
  }
  if (now <= 1600000000) { if (pump) { pump("Update: time sync failed"); delay(1500); } return false; }
  DateTime local = utcToLocal(now, geo);
  if (!rtcWrite(local)) { if (pump) { pump("Update: time sync failed"); delay(1500); } return false; }
  char msg[40];
  snprintf(msg, sizeof msg, "Update: time synced %02u:%02u", local.h, local.min);
  if (pump) { pump(msg); delay(1500); }
  return true;
}
#endif  // ARDUINO
