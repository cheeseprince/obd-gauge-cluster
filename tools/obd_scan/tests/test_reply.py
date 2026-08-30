import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2]))

from obd_scan.reply import Cls, assemble_multiframe, classify, decode_supported


def test_positive_requires_echo_match():
    r = classify("62F4461A\r", "22F446")
    assert r.cls is Cls.POSITIVE
    assert r.payload == bytes([0x1A])


def test_wrong_echo_is_not_positive():
    # Reply echoes a DIFFERENT pid than requested -> must not count as a hit.
    r = classify("62F4471A\r", "22F446")
    assert r.cls is not Cls.POSITIVE


def test_negative_response_out_of_range():
    r = classify("7F2231\r", "22F446")
    assert r.cls is Cls.NRC_OUT_OF_RANGE
    assert r.nrc == 0x31


def test_reply_containing_62_is_not_a_false_hit():
    # The firmware scanner searched for the substring "62" anywhere in the
    # reply. Here the NRC byte itself renders as 62, so the old approach would
    # have called this a hit. It is a negative response.
    r = classify("7F2262\r", "22F446")
    assert r.cls is Cls.NRC_OTHER
    assert r.nrc == 0x62


def test_no_data():
    assert classify("NO DATA\r", "22F446").cls is Cls.NO_DATA


def test_empty_is_no_data():
    assert classify("", "22F446").cls is Cls.NO_DATA


def test_elm_errors():
    for txt in ["?", "BUFFER FULL", "CAN ERROR", "STOPPED", "UNABLE TO CONNECT"]:
        assert classify(txt + "\r", "22F446").cls is Cls.ELM_ERROR


def test_pending_and_other_nrc():
    assert classify("7F2278\r", "22F446").cls is Cls.NRC_PENDING
    assert classify("7F2211\r", "22F446").cls is Cls.NRC_OTHER


def test_mode01_positive_echo():
    # Mode 01 positive response is 0x41 + pid.
    r = classify("410C1AF8\r", "010C")
    assert r.cls is Cls.POSITIVE
    assert r.payload == bytes([0x1A, 0xF8])


def test_spaces_are_tolerated():
    r = classify("62 F4 46 1A\r", "22F446")
    assert r.cls is Cls.POSITIVE
    assert r.payload == bytes([0x1A])


def test_multiframe_assembly():
    # ELM multi-frame form: total length line, then N:<hex> fragments.
    # NOTE: brief specified header "00D" (13), but the three fragments below
    # sum to exactly 12 bytes (6 + 5 + 1) -- verified via bytes.fromhex() on
    # each fragment. Declaring 13 makes assemble_multiframe's own "reject if
    # under-length" rule (required by test_multiframe_rejects_dropped_fragment
    # below, using this same fixture) correctly return b"" for a fixture that
    # was meant to assemble cleanly. Corrected the header to "00C" (12) to
    # match the actual fragment byte count; the fragment hex is untouched.
    lines = ["00C", "0:62F4780706E1", "1:0777077700", "2:00"]
    out = assemble_multiframe(lines)
    assert out[:3] == bytes([0x62, 0xF4, 0x78])
    assert len(out) == 0x0C


def test_multiframe_truncates_to_declared_length():
    lines = ["005", "0:62F4461A", "1:FFFFFFFF"]
    assert len(assemble_multiframe(lines)) == 5


def test_multiframe_rejects_dropped_fragment():
    # Declared 0x0D bytes but fragments only supply 7 -> under-length, reject.
    lines = ["00D", "0:62F4780706E1"]
    assert assemble_multiframe(lines) == b""


def test_multiframe_truncation_discards_55_padding():
    # ELM pads a short final frame with 0x55. Padding always sits beyond the
    # declared length, so truncating the concatenated body to `total` is
    # sufficient to discard it -- no separate rstrip step is needed (or
    # correct: an rstrip would also eat a genuine final 0x55 data byte).
    lines = ["004", "0:62F4461A55", "1:555555"]
    assert assemble_multiframe(lines) == bytes([0x62, 0xF4, 0x46, 0x1A])


def test_multiframe_genuine_0x55_final_byte_is_not_rejected():
    # The genuine last data byte IS 0x55 and the declared length covers it.
    # A blind rstrip(b"\x55") before the length check would strip this real
    # byte, fail the length guard, and return b"" -- a missed discovery on
    # a perfectly good reply. Truncation to the declared length is the only
    # thing that should ever discard trailing bytes.
    lines = ["004", "0:62F44655"]
    assert assemble_multiframe(lines) == bytes([0x62, 0xF4, 0x46, 0x55])


def test_multiframe_rejects_duplicate_and_missing_index():
    # Fragment 1 arrives twice and fragment 2 never arrives, but the
    # concatenated length (6 + 5 + 5 = 16... here contrived to exactly hit
    # total) coincidentally reaches the declared total. Indices 0,1,1 are
    # not a complete 0..N-1 set -- must be rejected, not silently accepted
    # with a duplicated fragment substituting for the missing one.
    lines = ["00C", "0:62F4780706E1", "1:0777077700", "1:0777077700"]
    assert assemble_multiframe(lines) == b""


# The ISO-TP sequence number is a FOUR-BIT field. The ELM prints its low nibble,
# so a reply of 17 or more frames counts 0..F and then wraps back to 0 -- our own
# tools/hil/emulator iso_tp_frames says exactly that in its docstring, and these
# fixtures were generated with it. A reply longer than 111 bytes cannot be read
# without handling the wrap, and classify() is on the elm.py read path, so
# census / sweep / discover / log all depend on this.
_WRAPPED_17_FRAMES = [
    "070",                       # 0x70 = 112 bytes
        "0:620077010203",
        "1:0405060708090A",
        "2:0B0C0D0E0F1011",
        "3:12131415161718",
        "4:191A1B1C1D1E1F",
        "5:20212223242526",
        "6:2728292A2B2C2D",
        "7:2E2F3031323334",
        "8:35363738393A3B",
        "9:3C3D3E3F404142",
        "A:43444546474849",
        "B:4A4B4C4D4E4F50",
        "C:51525354555657",
        "D:58595A5B5C5D5E",
        "E:5F606162636465",
        "F:666768696A6B6C",
        "0:6D",
]


def test_multiframe_accepts_wrapped_sequence_index():
    out = assemble_multiframe(_WRAPPED_17_FRAMES)
    assert len(out) == 0x70
    assert out[:3] == bytes([0x62, 0x00, 0x77])
    assert out[-1] == 0x6D          # the byte carried by the wrapped frame


def test_multiframe_rejects_duplicate_after_the_wrap():
    # Widening the index check must not become "accept anything". Frame 0
    # arrives, wraps to 0 correctly, and then repeats -- the second copy sits
    # where sequence 1 belongs, so the reply is still corrupt and still refused.
    lines = _WRAPPED_17_FRAMES + ["0:6D"]
    assert assemble_multiframe(lines) == b""


def test_classify_handles_multiframe_positive():
    # Header corrected to "00C" (12) for the same reason as
    # test_multiframe_assembly above -- these fragments sum to 12 bytes.
    raw = "00C\r0:62F4780706E1\r1:0777077700\r2:00\r"
    r = classify(raw, "22F478")
    assert r.cls is Cls.POSITIVE
    assert r.payload[0] == 0x07          # sensor-present mask


def test_decode_supported_bitmap():
    # 0x80000000 at base 0x00 -> PID 0x01 supported and nothing else.
    assert decode_supported(bytes([0x80, 0x00, 0x00, 0x00]), 0x00) == [0x01]
    # All bits at base 0x20 -> PIDs 0x21..0x40.
    got = decode_supported(bytes([0xFF, 0xFF, 0xFF, 0xFF]), 0x20)
    assert got[0] == 0x21 and got[-1] == 0x40 and len(got) == 32


def test_decode_supported_rejects_short_payload():
    assert decode_supported(bytes([0x80, 0x00]), 0x00) == []
