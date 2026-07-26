// test_ntp_time: the pure UTC->local conversion (tzStd offset + US DST rule).
#include <cassert>
#include <cstdio>
#include <ctime>
#include "../src/ntp_time.h"

// Build a UTC epoch from Y/M/D h:m:s using timegm (UTC, no local tz influence).
static time_t utc(int y, int mon, int d, int h, int mi, int s) {
  struct tm t{}; t.tm_year = y - 1900; t.tm_mon = mon - 1; t.tm_mday = d;
  t.tm_hour = h; t.tm_min = mi; t.tm_sec = s;
  return timegm(&t);
}

int main() {
  GeoLocation pst; pst.lat = 37.0f; pst.lon = -122.0f; pst.tzStd = -8;

  // July → PDT (UTC-7): 2026-07-01 20:00 UTC = 13:00 local.
  DateTime summer = utcToLocal(utc(2026, 7, 1, 20, 0, 0), pst);
  assert(summer.h == 13 && summer.mon == 7 && summer.d == 1);

  // January → PST (UTC-8): 2026-01-01 20:00 UTC = 12:00 local.
  DateTime winter = utcToLocal(utc(2026, 1, 1, 20, 0, 0), pst);
  assert(winter.h == 12 && winter.mon == 1 && winter.d == 1);

  // Day rollover under a negative offset: 2026-01-01 03:00 UTC, PST(-8) → prev
  // day 19:00 (2025-12-31).
  DateTime rollback = utcToLocal(utc(2026, 1, 1, 3, 0, 0), pst);
  assert(rollback.y == 2025 && rollback.mon == 12 && rollback.d == 31 && rollback.h == 19);

  printf("test_ntp_time: ALL PASS\n");
  return 0;
}
