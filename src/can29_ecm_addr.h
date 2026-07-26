#pragma once
#include <cstdint>
// 29-bit ECM diagnostic address for the GM WiFi/29-bit path. Default 0x10; a
// future WiFi-adapter hunt may store the truck's real address here at init.
// Shared by the query engine and the GM profile's emit; host-clean (no device deps).
inline uint8_t& can29EcmAddr() { static uint8_t a = 0x10; return a; }
