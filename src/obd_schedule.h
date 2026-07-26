#pragma once

// Tiered scheduler over the readout table. next() returns the index of the next
// readout to query (fast tier ~4x slow; rare tier, e.g. baro, occasionally).
// Buckets are grouped by ELM header (header changes cost an AT SH round trip).
// Pure: builds its tier lists from READOUTS[].tier at construction.
class ObdSchedule {
 public:
  ObdSchedule();
  // (Re)build the tier lists from the ACTIVE profile's READOUTS. MUST be called
  // after g_activeProfile is set — from the OBD source's begin(), which setup()
  // calls after profile selection. The ctor deliberately does NOT read the
  // profile: an ObdSchedule is a member of a file-scope OBD source and is
  // constructed during static init, when g_activeProfile is still null. Idempotent.
  void build();
  int next();          // readout index to query
 private:
  static void sortByHeader(int* bucket, int n);   // stable, by READOUTS[].header
  int fast_[16]; int nFast_ = 0;
  int slow_[16]; int nSlow_ = 0;
  int rare_[16]; int nRare_ = 0;
  int tick_ = 0, fi_ = 0, si_ = 0, ri_ = 0;
};
