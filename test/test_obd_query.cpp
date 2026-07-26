// test_obd_query: exercise the multi-step header walk in pidQueryStep on the
// host. Stubs portMUX so the device-only engine compiles, feeds ELM '>' acks
// through a fake IO, and asserts the setup+query write sequence for a header
// change matches the legacy atShFor() oracle (GM = single setup command).
#include <cassert>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>
#define portMUX_TYPE int
#define portENTER_CRITICAL(x)
#define portEXIT_CRITICAL(x)
#include "../src/obd_query.h"
#include "../src/vehicle_active.h"

extern const VehicleProfile GM_SIERRA_LZ0_PROFILE;

struct FakeIO {
  std::vector<std::string> writes;
  std::string inbox;
  void write(const char* s) { writes.push_back(s); }
  int  available()          { return (int)inbox.size(); }
  int  read()               { int c = (unsigned char)inbox[0]; inbox.erase(inbox.begin()); return c; }
  void flushRx()            { inbox.clear(); }
};

int main() {
  g_activeProfile = &GM_SIERRA_LZ0_PROFILE;
  PidQueryState q;
  ObdSchedule sched;
  sched.build();
  float values[STAT_COUNT] = {0};
  ObdReadings cur{};
  portMUX_TYPE mux = 0;
  FakeIO io;

  // Tick 1: Idle picks a readout and emits its header's first setup command.
  pidQueryStep(q, sched, io, /*now*/0, /*timeout*/50, values, cur, mux, /*can29*/false);
  assert(q.txState == Tx::WaitHeader);
  int idx = q.curIdx;
  int hdr = READOUTS[idx].header;
  // Oracle: what the legacy atShFor() would have written for this header.
  const char* wantSh = hdr == 1 ? "AT SH 7E2\r" : hdr == 2 ? "AT SH 7E0\r" : "AT SH 7DF\r";
  assert(io.writes.size() == 1 && io.writes.back() == wantSh);

  // Tick 2: ack the header; the (single-step GM) sequence ends, query is sent.
  io.inbox = ">";
  pidQueryStep(q, sched, io, 1, 50, values, cur, mux, false);
  assert(q.txState == Tx::WaitData);
  char wantCmd[16]; snprintf(wantCmd, sizeof wantCmd, "%s\r", READOUTS[idx].cmd);
  assert(io.writes.back() == wantCmd);

  printf("test_obd_query: ALL PASS\n");
  return 0;
}
