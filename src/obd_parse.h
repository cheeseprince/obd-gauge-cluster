#pragma once
#include <cstddef>   // size_t
#include <cstdint>
#include <string>
#include <vector>

// Tolerant parse of an ELM327 ASCII reply into the PID's data bytes.
// Handles spaces, CR/LF, the '>' prompt, a "SEARCHING..." preamble, and the
// echoed service/PID bytes. Validates the service byte (mode | 0x40) and the
// echoed PID, then returns the payload bytes (A, B, ...).
// Returns false on "NO DATA"/"?"/empty/too-short/mismatch.
//   mode: 0x01 for standard PIDs (pid low byte echoed),
//         0x22 for Mode 22 (16-bit pid echoed).
bool parseObdResponse(const std::string& resp, uint8_t mode, uint16_t pid,
                      std::vector<uint8_t>& data);
