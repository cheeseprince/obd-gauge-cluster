#pragma once
#include <cstdint>
#include <cstddef>

// Fixed-capacity FIFO byte ring. The BLE notify callback push()es reply bytes;
// the ELM reply reader read()s until the '>' prompt. Single-producer/single-
// consumer; on-device the producer is the NimBLE notify callback. CAP covers the
// longest ELM reply (multi-line mode 22) with margin.
class ByteRing {
 public:
  static const size_t CAP = 512;
  bool   push(uint8_t b);      // false if full (byte dropped)
  size_t available() const;    // bytes ready to read
  int    read();               // next byte, or -1 if empty
  int    peek() const;         // next byte without consuming, or -1 if empty
  void   clear();              // drop all buffered bytes
 private:
  uint8_t buf_[CAP];
  size_t  head_ = 0;           // next write index
  size_t  tail_ = 0;           // next read index
  size_t  count_ = 0;
};
