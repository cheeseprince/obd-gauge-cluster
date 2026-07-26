#pragma once
// ble_rank.h — pure, host-testable ranking of scanned BLE devices for OBD
// adapter discovery. Deliberately free of NimBLE/Arduino dependencies so the
// connect-order logic can be unit-tested on the host (see test/test_ble_rank.cpp).

// One scanned device: its advertised name (may be empty) and RSSI.
struct BleCand {
  const char* name;   // advertised name; nullptr or "" if the device has none
  int         rssi;   // signal strength in dBm (negative; stronger = larger)
};

// True when the advertised name hints at an OBD-II / ELM327 adapter
// (case-insensitive substring match against a small hint list).
bool bleNameLooksLikeObd(const char* name);

// Fill order[0..n-1] with indices into cands[], giving the order to try
// connecting: OBD-named adapters first (strongest RSSI first within that
// group), then every remaining device (strongest RSSI first). Stable — devices
// with equal rank keep their input order. order[] must have room for n ints.
void bleRankCandidates(const BleCand* cands, int n, int* order);
