#!/bin/bash -eu
# Build the OBD parse/decode fuzz target for ClusterFuzzLite.
#
# $CXX, $CXXFLAGS, $LIB_FUZZING_ENGINE and $OUT are provided by the OSS-Fuzz
# base image; CXXFLAGS already carries the sanitizer and coverage instrumentation
# for whichever sanitizer the workflow selected, so do NOT add -fsanitize here.
#
# The source list mirrors SRC_FUZZ in test/Makefile. If a decoder gains a new
# dependency, both lists need it — the host suite builds the same harness via
# fuzz_obd_driver.cpp, so a mismatch shows up there first as a link error.
cd "$SRC/obd-gauge-cluster"

$CXX $CXXFLAGS -std=c++17 -I src \
    test/fuzz_obd_libfuzzer.cpp \
    src/obd_parse.cpp \
    src/readouts.cpp \
    src/vehicles/gm_sierra_lz0.cpp \
    src/pid_decode.cpp \
    src/gauge_model.cpp \
    src/vehicle_active.cpp \
    $LIB_FUZZING_ENGINE \
    -o "$OUT/fuzz_obd"

# Seed corpus: the shapes that have historically broken the parser. Giving
# libFuzzer a foothold in the valid-frame grammar saves it discovering "0:"
# multi-frame framing from scratch.
mkdir -p "$OUT/fuzz_obd_seed_corpus"
i=0
while IFS= read -r line; do
    # 4 control bytes (mode 0x01, pid 0x009B, selector 0) then the reply text.
    printf '\x01\x00\x9b\x00%b' "$line" > "$OUT/fuzz_obd_seed_corpus/seed_$i"
    i=$((i + 1))
done <<'SEEDS'
>
OK\r\r>
NO DATA\r\r
SEARCHING...\r41 0C 1A F8\r>
41 0C 1A F8
62 00 9B 0E 84 41 E1
7F 22 31
00C\r0:62009B0E84\r\r
00C\r0:620083C3008E\r1:0004FFFFFFFF55\r\r
SEEDS
zip -jq "$OUT/fuzz_obd_seed_corpus.zip" "$OUT"/fuzz_obd_seed_corpus/* && rm -rf "$OUT/fuzz_obd_seed_corpus"
