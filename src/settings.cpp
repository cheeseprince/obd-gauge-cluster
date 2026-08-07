#include "settings.h"
#include "solar.h"

uint8_t nextBrightness(uint8_t cur) {
  if (cur < 25)  return 25;
  if (cur < 50)  return 50;
  if (cur < 75)  return 75;
  if (cur < 100) return 100;
  return 25;                      // wrap 100 -> 25
}

void toggleNight(Settings& s) {
  s.night = !s.night;
  s.brightnessPct = s.night ? 25 : 100;   // convenience default; overridable
}

void cycleNightMode(Settings& s, bool hasRtc) {
  switch (s.nightMode) {
    case NIGHT_AUTO:  s.nightMode = NIGHT_DAY;   break;
    case NIGHT_DAY:   s.nightMode = NIGHT_NIGHT; break;
    default:          s.nightMode = hasRtc ? NIGHT_AUTO : NIGHT_DAY; break;
  }
}

const char* nightModeLabel(const Settings& s) {
  return s.nightMode == NIGHT_AUTO ? "AUTO"
       : s.nightMode == NIGHT_NIGHT ? "ON" : "OFF";
}

bool resolveNight(const Settings& s, bool rtcValid, const DateTime& now, bool curNight) {
  switch (s.nightMode) {
    case NIGHT_DAY:   return false;
    case NIGHT_NIGHT: return true;
    default:          return (rtcValid && geoValid(s.geo)) ? isNight(now, s.geo) : curNight;
  }
}

#ifdef ARDUINO
#include <Preferences.h>
#include <cstring>

// Reuse the existing "obd" NVS namespace (keys distinct from bleaddr/bletype).
void loadSettings(Settings& s) {
  Preferences p;
  p.begin("obd", true);                          // read-only
  s.night         = p.getUChar("night", 0) != 0;
  s.nightMode     = p.getUChar("nightm", NIGHT_AUTO);
  s.brightnessPct = p.getUChar("bright", 100);
  s.metric        = p.getUChar("metric", 0) != 0;
  s.logging       = p.getUChar("log", 1) != 0;
  s.geo.lat       = p.getFloat("lat", 200.0f);   // 200 = unset sentinel
  s.geo.lon       = p.getFloat("lon", 0.0f);
  s.geo.tzStd     = p.getChar("tz", 0);
  String vk = p.getString("vehkey", "");
  strncpy(s.vehicleKey, vk.c_str(), sizeof s.vehicleKey - 1);
  s.vehicleKey[sizeof s.vehicleKey - 1] = '\0';
  s.vehicleAuto = p.getUChar("vehauto", 1) != 0;
  String dn = p.getString("detname", ""), de = p.getString("deteng", "");
  strncpy(s.detectedName, dn.c_str(), sizeof s.detectedName - 1);
  s.detectedName[sizeof s.detectedName - 1] = '\0';
  strncpy(s.detectedEngine, de.c_str(), sizeof s.detectedEngine - 1);
  s.detectedEngine[sizeof s.detectedEngine - 1] = '\0';
  p.end();
  if (s.brightnessPct == 0 || s.brightnessPct > 100) s.brightnessPct = 100;
  if (s.nightMode > NIGHT_NIGHT) s.nightMode = NIGHT_AUTO;
}

void saveSettings(const Settings& s) {
  Preferences p;
  p.begin("obd", false);
  p.putUChar("night",  s.night ? 1 : 0);
  p.putUChar("nightm", s.nightMode);
  p.putUChar("bright", s.brightnessPct);
  p.putUChar("metric", s.metric ? 1 : 0);
  p.putUChar("log",    s.logging ? 1 : 0);
  p.putFloat("lat", s.geo.lat);
  p.putFloat("lon", s.geo.lon);
  p.putChar("tz", (int8_t)s.geo.tzStd);
  p.putString("vehkey", s.vehicleKey);
  p.putUChar("vehauto", s.vehicleAuto ? 1 : 0);
  p.putString("detname", s.detectedName);
  p.putString("deteng",  s.detectedEngine);
  p.end();
}
#endif
