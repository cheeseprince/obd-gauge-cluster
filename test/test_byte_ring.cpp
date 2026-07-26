#include <cstdio>
#include "byte_ring.h"
static int failures=0;
static void check(bool c,const char*m){if(!c){printf("FAIL: %s\n",m);failures++;}}
int main(){
  ByteRing r;
  check(r.available()==0 && r.read()==-1 && r.peek()==-1, "empty: 0/-1/-1");

  // FIFO order
  check(r.push('4') && r.push('1') && r.push('>'), "push 3");
  check(r.available()==3, "available 3");
  check(r.peek()=='4', "peek does not consume");
  check(r.read()=='4' && r.read()=='1' && r.read()=='>', "FIFO read order");
  check(r.available()==0, "drained");

  // wrap-around: push/drain past CAP
  for (int i=0;i<ByteRing::CAP-1;i++) r.push((uint8_t)(i&0xFF));
  check(r.available()==ByteRing::CAP-1, "near-full count");
  check(r.read()==0, "wrap read first");
  check(r.push(0xAB), "push after read (wrap)");

  // full drops
  ByteRing f;
  size_t ok=0; for (size_t i=0;i<ByteRing::CAP+10;i++) if (f.push(1)) ok++;
  check(ok==ByteRing::CAP, "accepts exactly CAP, drops overflow");
  check(!f.push(1), "push on full returns false");

  // clear
  f.clear();
  check(f.available()==0 && f.read()==-1, "clear empties");

  printf(failures?"\n%d FAILED\n":"\nALL PASS\n",failures); return failures?1:0;
}
