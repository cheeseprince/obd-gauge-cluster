#include "byte_ring.h"

bool ByteRing::push(uint8_t b) {
  if (count_ >= CAP) return false;
  buf_[head_] = b;
  head_ = (head_ + 1) % CAP;
  count_++;
  return true;
}

size_t ByteRing::available() const { return count_; }

int ByteRing::read() {
  if (count_ == 0) return -1;
  uint8_t b = buf_[tail_];
  tail_ = (tail_ + 1) % CAP;
  count_--;
  return b;
}

int ByteRing::peek() const { return count_ == 0 ? -1 : buf_[tail_]; }

void ByteRing::clear() { head_ = tail_ = count_ = 0; }
