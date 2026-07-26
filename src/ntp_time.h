#pragma once
#include <ctime>
#include "solar.h"   // GeoLocation, isUsDst, geoValid
#include "rtc.h"     // DateTime

// Convert a UTC epoch to a local-time DateTime using the provisioned location's
// standard UTC offset (geo.tzStd) plus the US daylight-saving rule (isUsDst).
// Pure — no hardware, host-testable.
DateTime utcToLocal(time_t utc, const GeoLocation& geo);

// Device-only: query NTP (pool.ntp.org, fallback time.nist.gov), convert to
// local via utcToLocal(), and write the RTC. Emits "Update: ..." status via
// pump. Skips (returns false) if geo is unset or NTP doesn't answer in time.
// Never throws; the caller (OTA check) proceeds regardless of the return value.
bool ntpSyncRtc(const GeoLocation& geo, void (*pump)(const char* status));
