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
    vin="3GTUUEE8012345678",
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

SCENARIOS = {
    "generic": GENERIC,
    "gm_sierra": GM_SIERRA,
}
