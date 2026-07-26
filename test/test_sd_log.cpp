#include <cstdio>
#include <cstring>
#include "sd_log.h"
#include "readouts.h"
#include "app_types.h"
#include "../src/vehicle_active.h"

extern const VehicleProfile GM_SIERRA_LZ0_PROFILE;

static int failures = 0;
static void check(bool c, const char* m){ if(!c){ printf("FAIL: %s\n", m); failures++; } }
static bool has(const char* h, const char* n){ return strstr(h, n) != nullptr; }
static int commas(const char* s){ int c = 1; for (; *s; s++) if (*s == ',') c++; return c; }

int main() {
  g_activeProfile = &GM_SIERRA_LZ0_PROFILE;
  int disp = 0;
  for (int i = 0; i < STAT_COUNT; i++) if (isActive(i)) disp++;   // CSV = active (displayed + helpers)

  char buf[512];
  buildHeaderRow(buf, sizeof buf);
  check(strncmp(buf, "datetime,uptime_s,", 18) == 0, "header prefix");
  check(has(buf, "TRANS") && has(buf, "OIL") && has(buf, "BOOST"), "header has stat names");
  check(commas(buf) == disp + 2, "header column count = displayed + 2");
  check(buf[strlen(buf)-1] == '\n', "header ends with newline");

  float v[STAT_COUNT] = {0}; bool valid[STAT_COUNT] = {false};
  v[(int)StatId::Trans] = 235; valid[(int)StatId::Trans] = true;
  v[(int)StatId::Oil]   = 250; valid[(int)StatId::Oil]   = true;   // Boost/Coolant left invalid -> blank
  buildDataRow(DateTime{2026,7,1,14,30,12}, 142, v, valid, buf, sizeof buf);
  check(strncmp(buf, "2026-07-01 14:30:12,142,", 24) == 0, "data row timestamp + uptime");
  check(has(buf, ",235,"), "trans value present");
  check(has(buf, ",250,"), "oil value present");
  check(has(buf, ",,"), "invalid stat -> blank cell");
  check(commas(buf) == disp + 2, "data column count = displayed + 2");

  // buildLogPath: RTC-stamped name only when the RTC has actually been read.
  char path[48];
  buildLogPath(DateTime{2026,7,1,14,30,12}, true, 0, path, sizeof path);
  check(strcmp(path, "/logs/20260701_143012.csv") == 0, "rtcValid -> stamped filename");
  // rtcValid=false must fall back EVEN with a plausible date — rtcNow defaults to
  // 2026-01-01, so trusting y>=2000 alone reused one constant name (and truncated
  // the prior drive's log) whenever the knob/RTC was absent.
  buildLogPath(DateTime{2026,1,1,0,0,0}, false, 7, path, sizeof path);
  check(strcmp(path, "/logs/sess007.csv") == 0, "no RTC -> session filename");
  buildLogPath(DateTime{1970,1,1,0,0,0}, true, 8, path, sizeof path);
  check(strcmp(path, "/logs/sess008.csv") == 0, "pre-2000 date -> session filename");

  printf(failures ? "\n%d FAILED\n" : "\nALL PASS\n", failures);
  return failures ? 1 : 0;
}
