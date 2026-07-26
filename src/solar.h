#pragma once
#include "rtc.h"   // DateTime

// solar.h — sunrise/sunset + US-DST helpers for the auto night theme.
// Pure math (host-tested in test/test_solar.cpp); no device dependencies.
// Location is supplied by the caller (provisioned via the WiFi portal, stored
// in Settings/NVS) — nothing about a location is compiled in.

// A provisioned location. The unset sentinel is any out-of-range value; the
// default-constructed lat of 200.0f makes geoValid() false until set.
struct GeoLocation {
  float lat = 200.0f;   // degrees north (+N).  200 = unset sentinel.
  float lon = 0.0f;     // degrees east  (+E; west is negative)
  int   tzStd = 0;      // standard-time UTC offset (hours); US DST added on top
};

// True when lat/lon are within valid ranges — i.e. a location has been set.
bool geoValid(const GeoLocation& g);

// True if US daylight-saving time is in effect at the given LOCAL date/hour
// (rule since 2007: starts 2nd Sunday of March 02:00, ends 1st Sunday of
// November 02:00). The repeated 01:xx hour at fall-back is treated as DST.
bool isUsDst(int y, int mon, int d, int h);

// Sunrise/sunset for `loc` on the given date, as LOCAL wall-clock hours [0,24)
// (DST applied). Returns false only in the polar day/night case.
bool sunTimesLocal(int y, int mon, int d, const GeoLocation& loc,
                   float& riseH, float& setH);

// Is this local wall-clock time at night (before sunrise / after sunset) at loc?
bool isNight(const DateTime& local, const GeoLocation& loc);
