#include <cstdio>
#include "alarm_ack.h"
static int failures = 0;
static void check(bool c, const char* m){ if(!c){ printf("FAIL: %s\n", m); failures++; } }

int main() {
  AlarmAck a;
  check(a.shouldShow(false) == false, "no alarm -> hidden");
  check(a.shouldShow(true)  == true,  "alarm active -> shown");
  check(a.shouldShow(true)  == true,  "still shown until acked");
  a.ack();
  check(a.shouldShow(true)  == false, "acked -> hidden while active");
  check(a.shouldShow(true)  == false, "stays hidden while still active");
  check(a.shouldShow(false) == false, "all clear -> hidden (and re-arms)");
  check(a.shouldShow(true)  == true,  "fresh alarm after clear -> shown again");
  printf(failures ? "\n%d FAILED\n" : "\nALL PASS\n", failures);
  return failures ? 1 : 0;
}
