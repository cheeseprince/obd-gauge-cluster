#pragma once
// ble_rank.h — pure, host-testable ranking of scanned BLE devices for OBD
// adapter discovery. Deliberately free of NimBLE/Arduino dependencies so the
// connect-order logic can be unit-tested on the host (see test/test_ble_rank.cpp).

// What the ADVERTISEMENT said about services, before we connect to anything.
//
// The three values are not symmetric, and that is the point:
//   Obd    the advert carries one of the service UUIDs this firmware speaks
//          (0x18f0 / 0xfff0 / 0xffe0) -- as good as identification gets without
//          connecting, so try it first
//   None   the advert carries NO service UUIDs. Says nothing either way, so the
//          device must still be tried; skipping these would strand any adapter
//          that does not advertise its service
//   Other  the advert carries service UUIDs and NONE of them are ours. This is a
//          positive statement that the device is something else, and is the only
//          case safe to skip
//
// Measured in a real RF environment 2026-08-05: 15 devices in range, 6
// advertising service UUIDs, 9 advertising none. The project's vLinker
// advertises 0x18f0, which is what makes preferring a match safe in practice.
enum class SvcHint : unsigned char { None = 0, Obd = 1, Other = 2 };

// One scanned device: its advertised name (may be empty), RSSI, and what its
// advertisement claimed about services.
struct BleCand {
  const char* name;   // advertised name; nullptr or "" if the device has none
  int         rssi;   // signal strength in dBm (negative; stronger = larger)
  SvcHint     svc = SvcHint::None;   // defaults to "said nothing" -- the safe value
};

// True when this device positively advertised a service set that is not ours,
// and can therefore be skipped WITHOUT connecting to it.
//
// Why this matters (UX-6): the connect path used to connect-then-inspect, so it
// GATT-connected to strangers' phones and watches and attempted bonding with
// them -- 87 connect failures and 6 stranger bonds in one 7-minute bench window
// with no adapter reachable. Every device this skips is a bond never attempted
// with somebody else's hardware.
bool bleShouldSkip(const BleCand& c);

// True when the advertised name hints at an OBD-II / ELM327 adapter
// (case-insensitive substring match against a small hint list).
bool bleNameLooksLikeObd(const char* name);

// Fill order[0..n-1] with indices into cands[], giving the order to try
// connecting: OBD-named adapters first (strongest RSSI first within that
// group), then every remaining device (strongest RSSI first). Stable — devices
// with equal rank keep their input order. order[] must have room for n ints.
void bleRankCandidates(const BleCand* cands, int n, int* order);
