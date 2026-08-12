#!/usr/bin/env python3
"""Built-in fake vehicles for the ELM327 responder.

EVERY VALUE HERE IS A SYNTHETIC FIXTURE, not a reading from a real vehicle.
They are chosen to be unambiguous on screen: a wrong decode should look
obviously wrong (750 rpm, 50 °C) rather than plausible, because a fixture whose
errors look reasonable teaches the rig nothing.

THE VIN IS SYNTHETIC AND ALLOWLISTED. `scripts/check_no_pii.py` fails CI on any
VIN-shaped token that is not in its allowlist, which is what keeps a real VIN
out of this repository. Do not paste one here to "make it more realistic".

Adding a vehicle: give it a VIN whose WMI and discriminator positions match the
profile you want the dash to auto-select (see `docs/VEHICLES.md`), then add the
enhanced DIDs under the CAN header the real ECU answers on. The header split is
the point — see the note on `gm_sierra` below.
"""

from elm_responder import Scenario

# Standard Mode-01 PIDs, shared by every scenario. Keys are the PID byte(s) as
# they appear after the mode; values are the payload the ECU returns.
_COMMON_MODE01 = {
    "00": "BE3FA813",   # supported PIDs 01-20
    "04": "41",         # calculated engine load
    "05": "5A",         # coolant temp, A-40 = 50 C
    "0B": "64",         # intake manifold pressure
    "0C": "0BB8",       # engine rpm, ((A*256)+B)/4 = 750
    "0D": "37",         # vehicle speed, 55 km/h
    "0F": "3C",         # intake air temp, A-40 = 20 C
    "10": "0BB8",       # MAF
    "11": "32",         # throttle position
    "1F": "0708",       # run time since start
    "2F": "80",         # fuel level, 50%
    "33": "65",         # absolute barometric pressure
    "42": "3A98",       # control module voltage, 15.0 V
    "5C": "5A",         # engine oil temp, A-40 = 50 C
}

# A plain OBD-II vehicle: standard PIDs only, no enhanced data, and a VIN whose
# WMI is not in the firmware's table — so the dash must fall back to Generic.
# That fallback is worth testing: it is the path every unrecognised car takes.
GENERIC = Scenario(
    vin="JHM0123456789ABCD",
    mode01=dict(_COMMON_MODE01),
    mode22={},
)

# GM Sierra 1500 3.0L Duramax (LZ0).
#
# The VIN matches the LZ0 discriminator ([NPRUV] at vin[3], [HU] at vin[4], '8'
# at vin[7]), so a correct dash auto-selects `gm_sierra_lz0` and starts polling
# the enhanced set. That makes this scenario an end-to-end test of VIN parsing,
# profile selection AND enhanced polling in one run.
#
# NOTE THE TWO HEADERS. 221940 (transmission fluid temperature) answers on the
# TRANSMISSION module at 7E2, not the engine module at 7E0. A firmware bug that
# forgot to switch headers would read nothing here — which is exactly the class
# of bug a single-ECU fixture would hide.
GM_SIERRA = Scenario(
    vin="3GTUUEE80S2345678",
    mode01=dict(_COMMON_MODE01),
    mode22={
        "7E0": {                 # engine module
            "000C": "0BB8",      # rpm
            "005C": "5A",        # oil temperature
            "009B": "0064",      # DEF level
            "0078": "0190",      # EGT bank 1 sensor 1
            "007A": "01A0",      # EGT bank 1 sensor 2
            "0023": "2710",      # fuel rail pressure
            "0010": "05DC",      # mass air flow
            "0083": "0100",      # DEF-related
            "115C": "5A",        # oil-temperature candidate
        },
        "7E2": {                 # transmission module
            "1940": "008C",      # transmission fluid temperature — THE crux
            "199A": "04",        # current gear (prime candidate)
            "1995": "04",        # current gear (alternate candidate)
        },
    },
)

# Ford F-350 Super Duty 6.7L Power Stroke (10R140), 2020-26.
#
# The VIN matches `fordSuperDuty67_10R140`: '8' at vin[3] (F-350), 'W' at vin[4]
# (Super Duty platform), 'B'/'T' at vin[6..7] (6.7L Power Stroke) and 'N' at
# vin[9] (inside the LMNPRST model-year gate). So a correct dash auto-selects
# `ford_sd_67`.
#
# THIS TRUCK IS THE INVERSE OF THE SIERRA, WHICH IS WHY IT EARNS A SCENARIO.
# It is 11-bit ONLY -- a real scan found all fifteen 29-bit headers silent -- and
# almost everything it exposes is a STANDARD Mode-01 PID rather than an enhanced
# DID. Only transmission fluid temperature and current gear are enhanced, and
# both live on the TRANSMISSION module at 7E1 (not 7E2 like GM). A dash that
# hard-codes GM's header map reads nothing here.
#
# The Mode-01 rows below are the ones this profile polls beyond the common set.
# They exist because the profile originally left them dark after looking for
# them under the WRONG PID numbers (0111 pedal, 0123 rail, 012C EGR, 015E fuel
# rate) and concluding the parameters were unavailable. The truck serves them at
# 0149, 016D, 0169 and 019D. "PID X is not in the bitmap" does not mean "the
# parameter is unavailable" -- that lesson is what this fixture pins.
FORD_SD_67 = Scenario(
    vin="1FT8W3BT0N2345678",
    mode01={
        **_COMMON_MODE01,
        "43": "0064",              # absolute load
        "46": "3C",                # ambient air temp, A-40 = 20 C
        "49": "80",                # accelerator pedal D, 128/255 = 50%
        "62": "96",                # actual engine torque, A-125 = 25%
        "63": "03E8",              # reference torque = 1000 Nm
        # Fuel pressure control. bytes[3..4] are the ACTUAL rail pressure in
        # 10 kPa units: 0x35DD = 13789 -> 137.9 MPa -> ~20000 psi.
        "6D": "0735DD35DD000000000000",
        "69": "07400000000000",    # commanded EGR, byte[1] 0x40 = 25%
        # Charge air cooler temp: byte[1] is the reading, A-40 = 40 C.
        "77": "0150000000",
        # EGT bank 1, four sensors. Sensor 1 is the one the profile shows.
        "78": "0F03A6039102CC02B4",
        # DPF bank 1. bytes[3..4] = 0x01F4 = 500 -> 5.00 kPa differential.
        "7A": "0201E001F4FFFF",
        # NOx sensor. bytes[1..2] = 0x0064 = 100 ppm. NOT 0xFFFF, which is the
        # sensor-not-ready sentinel a cold truck really does report.
        "83": "030064000000000000",
        # Intake manifold absolute pressure A: bytes[1..2] x 0.03125 kPa.
        # 0x1900 = 6400 -> 200 kPa absolute -> ~14.5 psi of boost over baro.
        "87": "0119000000",
        # DEF sensor data. byte[1] is tank level: 0xBF = 191 -> 74.9%.
        "9B": "3FBF4095",
        # Engine fuel rate. bytes[0..1] x 0.02 g/s = 1.74 g/s -> ~2 gal/hr.
        "9D": "00570057",
        "9E": "022F",              # engine exhaust flow rate
        "A1": "030064000000000000",  # NOx corrected
    },
    mode22={
        "7E1": {                   # TRANSMISSION module -- 7E1, not GM's 7E2
            "1E1C": "05A0",        # ATF temperature, SIGNED int16 / 16 = 90 C.
                                   # Must decode signed: the unsigned form reads
                                   # ~4096 C on a sub-zero cold start.
            "1E60": "06",          # current gear, 6 of 10 on the 10R140
        },
    },
)

SCENARIOS = {
    "generic": GENERIC,
    "gm_sierra": GM_SIERRA,
    "ford_sd_67": FORD_SD_67,
}
