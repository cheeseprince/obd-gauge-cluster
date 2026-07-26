#include "solar.h"
#include <cmath>

// NOAA-style sunrise/sunset ("Almanac for Computers" algorithm, as published
// in Ed Williams' Aviation Formulary). Accuracy ~2-3 minutes — far tighter
// than the theme switch needs.

static constexpr float DEG = 3.14159265f / 180.0f;   // degrees -> radians
static constexpr float ZENITH = 90.8333f;            // official sunrise/set (incl. refraction)

bool geoValid(const GeoLocation& g) {
  return g.lat >= -90.0f && g.lat <= 90.0f &&
         g.lon >= -180.0f && g.lon <= 180.0f;
}

// Day of week, 0=Sunday (Sakamoto's method).
static int dow(int y, int m, int d) {
  static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (m < 3) y--;
  return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

// Date (1..) of the Nth Sunday of the month (n = 1 or 2 here).
static int nthSunday(int y, int mon, int n) {
  int first = dow(y, mon, 1);                 // weekday of the 1st
  int firstSun = 1 + (7 - first) % 7;
  return firstSun + 7 * (n - 1);
}

bool isUsDst(int y, int mon, int d, int h) {
  if (mon > 3 && mon < 11) return true;       // Apr..Oct: always DST
  if (mon < 3 || mon > 11) return false;      // Dec..Feb: never
  if (mon == 3) {                             // starts 2nd Sunday 02:00
    int start = nthSunday(y, 3, 2);
    if (d != start) return d > start;
    return h >= 2;
  }
  int end = nthSunday(y, 11, 1);              // Nov: ends 1st Sunday 02:00
  if (d != end) return d < end;
  return h < 2;                               // repeated 01:xx counted as DST
}

static int dayOfYear(int y, int mon, int d) {
  int n = d;
  for (int m = 1; m < mon; m++) n += daysInMonth(y, (uint8_t)m);
  return n;
}

static float wrap(float v, float range) {
  while (v < 0)      v += range;
  while (v >= range) v -= range;
  return v;
}

// One sunrise (rising=true) or sunset event as UTC hours [0,24).
// Returns false if the sun never crosses the zenith that day (polar regions).
static bool sunEventUtc(int y, int mon, int d, bool rising,
                        const GeoLocation& loc, float& utH) {
  float N = (float)dayOfYear(y, mon, d);
  float lngHour = loc.lon / 15.0f;
  float t = N + (((rising ? 6.0f : 18.0f) - lngHour) / 24.0f);

  float M = 0.9856f * t - 3.289f;                        // mean anomaly (deg)
  float L = M + 1.916f * sinf(M * DEG) + 0.020f * sinf(2 * M * DEG) + 282.634f;
  L = wrap(L, 360.0f);                                   // true longitude (deg)

  float RA = atanf(0.91764f * tanf(L * DEG)) / DEG;      // right ascension (deg)
  RA = wrap(RA, 360.0f);
  RA += floorf(L / 90.0f) * 90.0f - floorf(RA / 90.0f) * 90.0f;  // same quadrant as L
  RA /= 15.0f;                                           // -> hours

  float sinDec = 0.39782f * sinf(L * DEG);
  float cosDec = cosf(asinf(sinDec));
  float cosH = (cosf(ZENITH * DEG) - sinDec * sinf(loc.lat * DEG)) /
               (cosDec * cosf(loc.lat * DEG));
  if (cosH > 1.0f || cosH < -1.0f) return false;         // never rises / never sets

  float H = acosf(cosH) / DEG;
  if (rising) H = 360.0f - H;
  H /= 15.0f;

  float T = H + RA - 0.06571f * t - 6.622f;              // local mean time
  utH = wrap(T - lngHour, 24.0f);
  return true;
}

bool sunTimesLocal(int y, int mon, int d, const GeoLocation& loc,
                   float& riseH, float& setH) {
  float riseUt, setUt;
  if (!sunEventUtc(y, mon, d, true, loc, riseUt) ||
      !sunEventUtc(y, mon, d, false, loc, setUt))
    return false;
  // DST decided at midday — the offset can't flip between dawn and dusk in a
  // way that matters for a theme switch.
  int tz = loc.tzStd + (isUsDst(y, mon, d, 12) ? 1 : 0);
  riseH = wrap(riseUt + (float)tz, 24.0f);
  setH  = wrap(setUt + (float)tz, 24.0f);
  return true;
}

bool isNight(const DateTime& local, const GeoLocation& loc) {
  float riseH, setH;
  if (!sunTimesLocal(local.y, local.mon, local.d, loc, riseH, setH))
    return false;                                        // polar edge case; stay day
  float h = (float)local.h + (float)local.min / 60.0f;
  return h < riseH || h >= setH;
}
