# Host-side unit tests — no PlatformIO or hardware required.
#   make        build and run all tests
#   make clean  remove the binaries
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -I../src

BIN_PID   = test_pid_decode
SRC_PID   = test_pid_decode.cpp ../src/pid_decode.cpp

BIN_PARSE = test_obd_parse
SRC_PARSE = test_obd_parse.cpp ../src/obd_parse.cpp

BIN_GAUGE = test_gauge_model
SRC_GAUGE = test_gauge_model.cpp ../src/gauge_model.cpp

BIN_MOCK  = test_mock_obd_source
SRC_MOCK  = test_mock_obd_source.cpp ../src/mock_obd_source.cpp ../src/gauge_model.cpp ../src/readouts.cpp ../src/vehicles/gm_sierra_lz0.cpp ../src/pid_decode.cpp ../src/vehicle_active.cpp

BIN_NAV   = test_nav_model
SRC_NAV   = test_nav_model.cpp ../src/nav_model.cpp ../src/readouts.cpp ../src/vehicles/gm_sierra_lz0.cpp ../src/pid_decode.cpp ../src/gauge_model.cpp ../src/vehicle_active.cpp

BIN_BTN   = test_button_nav
SRC_BTN   = test_button_nav.cpp ../src/button_nav.cpp ../src/nav_model.cpp ../src/readouts.cpp ../src/vehicles/gm_sierra_lz0.cpp ../src/pid_decode.cpp ../src/gauge_model.cpp ../src/vehicle_active.cpp

BIN_HIST  = test_history
SRC_HIST  = test_history.cpp ../src/history.cpp

BIN_SCHED = test_obd_schedule
SRC_SCHED = test_obd_schedule.cpp ../src/obd_schedule.cpp ../src/readouts.cpp ../src/vehicles/gm_sierra_lz0.cpp ../src/pid_decode.cpp ../src/gauge_model.cpp ../src/vehicle_active.cpp

BIN_RDT   = test_readouts
SRC_RDT   = test_readouts.cpp ../src/readouts.cpp ../src/vehicles/gm_sierra_lz0.cpp ../src/pid_decode.cpp ../src/gauge_model.cpp ../src/vehicle_active.cpp

BIN_PROF  = test_profile
SRC_PROF  = test_profile.cpp ../src/readouts.cpp ../src/vehicles/gm_sierra_lz0.cpp ../src/pid_decode.cpp ../src/gauge_model.cpp ../src/vehicle_active.cpp

BIN_ADDR  = test_addressing
SRC_ADDR  = test_addressing.cpp ../src/readouts.cpp ../src/vehicles/gm_sierra_lz0.cpp ../src/pid_decode.cpp ../src/gauge_model.cpp ../src/vehicle_active.cpp

BIN_OQ    = test_obd_query
SRC_OQ    = test_obd_query.cpp ../src/obd_schedule.cpp ../src/obd_parse.cpp ../src/readouts.cpp ../src/vehicles/gm_sierra_lz0.cpp ../src/pid_decode.cpp ../src/gauge_model.cpp ../src/economy.cpp ../src/vehicle_active.cpp

BIN_PROFB = test_profile_bmw
SRC_PROFB = test_profile_bmw.cpp ../src/readouts.cpp ../src/vehicles/bmw_f10_535i.cpp ../src/pid_decode.cpp ../src/gauge_model.cpp ../src/vehicle_active.cpp

BIN_PROFG = test_profile_generic
SRC_PROFG = test_profile_generic.cpp ../src/vehicles/generic_obd.cpp ../src/pid_decode.cpp ../src/gauge_model.cpp ../src/vehicle_active.cpp

BIN_PROFA = test_profile_audi
SRC_PROFA = test_profile_audi.cpp ../src/readouts.cpp ../src/vehicles/audi_q5.cpp ../src/pid_decode.cpp ../src/gauge_model.cpp ../src/vehicle_active.cpp

BIN_PROFJ = test_profile_jeep
SRC_PROFJ = test_profile_jeep.cpp ../src/readouts.cpp ../src/vehicles/jeep_ws.cpp ../src/vehicles/standard_plus.cpp ../src/pid_decode.cpp ../src/gauge_model.cpp ../src/vehicle_active.cpp

BIN_VREG  = test_vehicle_registry
SRC_VREG  = test_vehicle_registry.cpp ../src/vehicle_registry.cpp ../src/vehicle_active.cpp ../src/vehicles/generic_obd.cpp ../src/vehicles/gm_sierra_lz0.cpp ../src/vehicles/bmw_f10_535i.cpp ../src/vehicles/audi_q5.cpp ../src/vehicles/jeep_ws.cpp ../src/vehicles/standard_plus.cpp ../src/pid_decode.cpp ../src/gauge_model.cpp

BIN_WDTHB = test_wdt_heartbeat
SRC_WDTHB = test_wdt_heartbeat.cpp

BIN_VIN   = test_vin
SRC_VIN   = test_vin.cpp ../src/vin.cpp ../src/vehicle_registry.cpp ../src/vehicle_active.cpp ../src/vehicles/generic_obd.cpp ../src/vehicles/gm_sierra_lz0.cpp ../src/vehicles/bmw_f10_535i.cpp ../src/vehicles/audi_q5.cpp ../src/vehicles/jeep_ws.cpp ../src/vehicles/standard_plus.cpp ../src/pid_decode.cpp ../src/gauge_model.cpp

BIN_LAYOUT = test_layout
SRC_LAYOUT = test_layout.cpp ../src/readouts.cpp ../src/vehicles/gm_sierra_lz0.cpp ../src/pid_decode.cpp ../src/gauge_model.cpp ../src/vehicle_active.cpp

BIN_HOLD  = test_alarm_holdoff
SRC_HOLD  = test_alarm_holdoff.cpp ../src/alarm_holdoff.cpp ../src/gauge_model.cpp

BIN_SWATCH = test_stack_watch
SRC_SWATCH = test_stack_watch.cpp ../src/stack_watch.cpp

BIN_ENC   = test_encoder_logic
SRC_ENC   = test_encoder_logic.cpp ../src/encoder_logic.cpp

BIN_BRANK = test_ble_rank
SRC_BRANK = test_ble_rank.cpp ../src/ble_rank.cpp

BIN_RING  = test_byte_ring
SRC_RING  = test_byte_ring.cpp ../src/byte_ring.cpp

BIN_ECON  = test_economy
SRC_ECON  = test_economy.cpp ../src/economy.cpp

BIN_ACK   = test_alarm_ack
SRC_ACK   = test_alarm_ack.cpp

BIN_SET   = test_settings
SRC_SET   = test_settings.cpp ../src/settings.cpp ../src/solar.cpp ../src/rtc.cpp

BIN_SOLAR = test_solar
SRC_SOLAR = test_solar.cpp ../src/solar.cpp ../src/rtc.cpp

BIN_CREDS = test_wifi_creds
SRC_CREDS = test_wifi_creds.cpp ../src/wifi_cred_store.cpp

BIN_OTAC  = test_ota_common
SRC_OTAC  = test_ota_common.cpp ../src/ota_common.cpp

BIN_MENU  = test_settings_menu
SRC_MENU  = test_settings_menu.cpp ../src/settings_menu.cpp

BIN_CONN  = test_conn_status
SRC_CONN  = test_conn_status.cpp ../src/conn_status.cpp

BIN_BOOT  = test_boot_banner
SRC_BOOT  = test_boot_banner.cpp ../src/boot_banner.cpp

BIN_RTC   = test_rtc
SRC_RTC   = test_rtc.cpp ../src/rtc.cpp

BIN_NTP   = test_ntp_time
SRC_NTP   = test_ntp_time.cpp ../src/ntp_time.cpp ../src/solar.cpp ../src/rtc.cpp

BIN_SDLOG = test_sd_log
SRC_SDLOG = test_sd_log.cpp ../src/sd_log.cpp ../src/rtc.cpp ../src/readouts.cpp ../src/vehicles/gm_sierra_lz0.cpp ../src/pid_decode.cpp ../src/gauge_model.cpp ../src/vehicle_active.cpp

# Fuzz + ASan/UBSan harness over the parse + decode path (hostile-input check).
BIN_FUZZ  = fuzz_obd
SRC_FUZZ  = fuzz_obd.cpp ../src/obd_parse.cpp ../src/readouts.cpp ../src/vehicles/gm_sierra_lz0.cpp ../src/pid_decode.cpp ../src/gauge_model.cpp ../src/vehicle_active.cpp
SANFLAGS  = -fsanitize=address,undefined -fno-sanitize-recover=all -g -O1

# The ClusterFuzzLite libFuzzer harness, built here with a plain main() driver so
# it compiles and runs WITHOUT clang/libFuzzer. This is a build-integrity gate,
# not fuzzing: it stops a refactor from silently breaking the harness, which
# would otherwise only surface inside the OSS-Fuzz Docker image as an
# infrastructure error instead of an obvious break in the PR that caused it.
BIN_LFUZZ = fuzz_obd_harness
SRC_LFUZZ = fuzz_obd_libfuzzer.cpp fuzz_obd_driver.cpp ../src/obd_parse.cpp ../src/readouts.cpp ../src/vehicles/gm_sierra_lz0.cpp ../src/pid_decode.cpp ../src/gauge_model.cpp ../src/vehicle_active.cpp

.PHONY: test clean fuzz test-ota-sign
test: $(BIN_PID) $(BIN_PARSE) $(BIN_GAUGE) $(BIN_MOCK) $(BIN_NAV) $(BIN_BTN) $(BIN_HIST) $(BIN_SCHED) $(BIN_RDT) $(BIN_HOLD) $(BIN_ENC) $(BIN_RING) $(BIN_ECON) $(BIN_ACK) $(BIN_LAYOUT) $(BIN_SET) $(BIN_MENU) $(BIN_CONN) $(BIN_RTC) $(BIN_SDLOG) $(BIN_SOLAR) $(BIN_CREDS) $(BIN_OTAC) $(BIN_PROF) $(BIN_ADDR) $(BIN_OQ) $(BIN_PROFB) $(BIN_PROFG) $(BIN_PROFA) $(BIN_PROFJ) $(BIN_VREG) $(BIN_NTP) $(BIN_BRANK) $(BIN_VIN) $(BIN_FUZZ) $(BIN_LFUZZ) $(BIN_BOOT) $(BIN_SWATCH) $(BIN_WDTHB)
	./$(BIN_PID)
	./$(BIN_PARSE)
	./$(BIN_GAUGE)
	./$(BIN_MOCK)
	./$(BIN_NAV)
	./$(BIN_BTN)
	./$(BIN_HIST)
	./$(BIN_SCHED)
	./$(BIN_RDT)
	./$(BIN_HOLD)
	./$(BIN_ENC)
	./$(BIN_RING)
	./$(BIN_ECON)
	./$(BIN_ACK)
	./$(BIN_LAYOUT)
	./$(BIN_SET)
	./$(BIN_MENU)
	./$(BIN_CONN)
	./$(BIN_RTC)
	./$(BIN_SDLOG)
	./$(BIN_SOLAR)
	./$(BIN_CREDS)
	./$(BIN_OTAC)
	./$(BIN_PROF)
	./$(BIN_ADDR)
	./$(BIN_OQ)
	./$(BIN_PROFB)
	./$(BIN_PROFG)
	./$(BIN_PROFA)
	./$(BIN_PROFJ)
	./$(BIN_VREG)
	./$(BIN_NTP)
	./$(BIN_BRANK)
	./$(BIN_VIN)
	./$(BIN_FUZZ)
	./$(BIN_LFUZZ)
	./$(BIN_BOOT)
	./$(BIN_SWATCH)
	./$(BIN_WDTHB)
	bash test_ota_sign.sh

# Standalone: build + run just the sanitized fuzzer.
fuzz: $(BIN_FUZZ)
	./$(BIN_FUZZ)

# Standalone: just the OTA manifest signing round-trip (shell + openssl, no
# compile step — skips cleanly if openssl isn't on PATH).
test-ota-sign:
	bash test_ota_sign.sh

$(BIN_FUZZ): $(SRC_FUZZ) ../src/readouts.h ../src/obd_parse.h ../src/app_types.h
	$(CXX) $(CXXFLAGS) $(SANFLAGS) $(SRC_FUZZ) -o $(BIN_FUZZ)

$(BIN_LFUZZ): $(SRC_LFUZZ) ../src/readouts.h ../src/obd_parse.h ../src/app_types.h
	$(CXX) $(CXXFLAGS) $(SANFLAGS) $(SRC_LFUZZ) -o $(BIN_LFUZZ)

$(BIN_RTC): $(SRC_RTC) ../src/rtc.h
	$(CXX) $(CXXFLAGS) $(SRC_RTC) -o $(BIN_RTC)

$(BIN_NTP): $(SRC_NTP) ../src/ntp_time.h ../src/solar.h ../src/rtc.h
	$(CXX) $(CXXFLAGS) $(SRC_NTP) -o $(BIN_NTP)

$(BIN_SDLOG): $(SRC_SDLOG) ../src/sd_log.h ../src/rtc.h ../src/readouts.h
	$(CXX) $(CXXFLAGS) $(SRC_SDLOG) -o $(BIN_SDLOG)

$(BIN_PID): $(SRC_PID) ../src/pid_decode.h
	$(CXX) $(CXXFLAGS) $(SRC_PID) -o $(BIN_PID)

$(BIN_PARSE): $(SRC_PARSE) ../src/obd_parse.h
	$(CXX) $(CXXFLAGS) $(SRC_PARSE) -o $(BIN_PARSE)

$(BIN_GAUGE): $(SRC_GAUGE) ../src/gauge_model.h
	$(CXX) $(CXXFLAGS) $(SRC_GAUGE) -o $(BIN_GAUGE)

$(BIN_MOCK): $(SRC_MOCK) ../src/mock_obd_source.h ../src/obd_source.h ../src/app_types.h ../src/readouts.h ../src/pid_decode.h
	$(CXX) $(CXXFLAGS) $(SRC_MOCK) -o $(BIN_MOCK)

$(BIN_NAV): $(SRC_NAV) ../src/nav_model.h ../src/app_types.h
	$(CXX) $(CXXFLAGS) $(SRC_NAV) -o $(BIN_NAV)

$(BIN_BTN): $(SRC_BTN) ../src/button_nav.h ../src/nav_model.h ../src/app_types.h
	$(CXX) $(CXXFLAGS) $(SRC_BTN) -o $(BIN_BTN)

$(BIN_HIST): $(SRC_HIST) ../src/history.h ../src/app_types.h
	$(CXX) $(CXXFLAGS) $(SRC_HIST) -o $(BIN_HIST)

$(BIN_SCHED): $(SRC_SCHED) ../src/obd_schedule.h ../src/readouts.h ../src/app_types.h
	$(CXX) $(CXXFLAGS) $(SRC_SCHED) -o $(BIN_SCHED)

$(BIN_RDT): $(SRC_RDT) ../src/readouts.h ../src/app_types.h
	$(CXX) $(CXXFLAGS) $(SRC_RDT) -o $(BIN_RDT)

$(BIN_PROF): $(SRC_PROF) ../src/readouts.h ../src/app_types.h
	$(CXX) $(CXXFLAGS) $(SRC_PROF) -o $(BIN_PROF)

$(BIN_ADDR): $(SRC_ADDR) ../src/vehicle_profile.h ../src/readouts.h
	$(CXX) $(CXXFLAGS) $(SRC_ADDR) -o $(BIN_ADDR)

$(BIN_OQ): $(SRC_OQ) ../src/obd_query.h ../src/obd_source.h
	$(CXX) $(CXXFLAGS) $(SRC_OQ) -o $(BIN_OQ)

$(BIN_PROFB): $(SRC_PROFB) ../src/readouts.h ../src/app_types.h ../src/vehicle_profile.h
	$(CXX) $(CXXFLAGS) $(SRC_PROFB) -o $(BIN_PROFB)

$(BIN_PROFG): $(SRC_PROFG) ../src/readouts.h ../src/app_types.h ../src/vehicle_profile.h
	$(CXX) $(CXXFLAGS) $(SRC_PROFG) -o $(BIN_PROFG)

$(BIN_PROFA): $(SRC_PROFA) ../src/readouts.h ../src/app_types.h ../src/vehicle_profile.h
	$(CXX) $(CXXFLAGS) $(SRC_PROFA) -o $(BIN_PROFA)

$(BIN_PROFJ): $(SRC_PROFJ) ../src/readouts.h ../src/app_types.h ../src/vehicle_profile.h
	$(CXX) $(CXXFLAGS) $(SRC_PROFJ) -o $(BIN_PROFJ)

$(BIN_VREG): $(SRC_VREG) ../src/vehicle_registry.h ../src/vehicle_profile.h ../src/readouts.h ../src/app_types.h
	$(CXX) $(CXXFLAGS) $(SRC_VREG) -o $(BIN_VREG)

$(BIN_WDTHB): $(SRC_WDTHB) ../src/wdt_kick.h
	$(CXX) $(CXXFLAGS) $(SRC_WDTHB) -o $(BIN_WDTHB)

$(BIN_VIN): $(SRC_VIN) ../src/vin.h ../src/vehicle_registry.h
	$(CXX) $(CXXFLAGS) $(SRC_VIN) -o $(BIN_VIN)

$(BIN_LAYOUT): $(SRC_LAYOUT) ../src/readouts.h ../src/app_types.h
	$(CXX) $(CXXFLAGS) $(SRC_LAYOUT) -o $(BIN_LAYOUT)

$(BIN_HOLD): $(SRC_HOLD) ../src/alarm_holdoff.h ../src/app_types.h
	$(CXX) $(CXXFLAGS) $(SRC_HOLD) -o $(BIN_HOLD)

$(BIN_SWATCH): $(SRC_SWATCH) ../src/stack_watch.h
	$(CXX) $(CXXFLAGS) $(SRC_SWATCH) -o $(BIN_SWATCH)

$(BIN_ENC): $(SRC_ENC) ../src/encoder_logic.h
	$(CXX) $(CXXFLAGS) $(SRC_ENC) -o $(BIN_ENC)

$(BIN_BRANK): $(SRC_BRANK) ../src/ble_rank.h
	$(CXX) $(CXXFLAGS) $(SRC_BRANK) -o $(BIN_BRANK)

$(BIN_RING): $(SRC_RING) ../src/byte_ring.h
	$(CXX) $(CXXFLAGS) $(SRC_RING) -o $(BIN_RING)

$(BIN_ECON): $(SRC_ECON) ../src/economy.h
	$(CXX) $(CXXFLAGS) $(SRC_ECON) -o $(BIN_ECON)

$(BIN_ACK): $(SRC_ACK) ../src/alarm_ack.h
	$(CXX) $(CXXFLAGS) $(SRC_ACK) -o $(BIN_ACK)

$(BIN_SET): $(SRC_SET) ../src/settings.h ../src/solar.h
	$(CXX) $(CXXFLAGS) $(SRC_SET) -o $(BIN_SET)

$(BIN_SOLAR): $(SRC_SOLAR) ../src/solar.h ../src/rtc.h
	$(CXX) $(CXXFLAGS) $(SRC_SOLAR) -o $(BIN_SOLAR)

$(BIN_CREDS): $(SRC_CREDS) ../src/wifi_cred_store.h
	$(CXX) $(CXXFLAGS) $(SRC_CREDS) -o $(BIN_CREDS)

$(BIN_OTAC): $(SRC_OTAC) ../src/ota_common.h
	$(CXX) $(CXXFLAGS) $(SRC_OTAC) -o $(BIN_OTAC)

$(BIN_MENU): $(SRC_MENU) ../src/settings_menu.h
	$(CXX) $(CXXFLAGS) $(SRC_MENU) -o $(BIN_MENU)

$(BIN_CONN): $(SRC_CONN) ../src/obd_source.h ../src/app_types.h
	$(CXX) $(CXXFLAGS) $(SRC_CONN) -o $(BIN_CONN)

$(BIN_BOOT): $(SRC_BOOT) ../src/boot_banner.h
	$(CXX) $(CXXFLAGS) $(SRC_BOOT) -o $(BIN_BOOT)

clean:
	rm -f $(BIN_PID) $(BIN_PARSE) $(BIN_GAUGE) $(BIN_MOCK) $(BIN_NAV) $(BIN_BTN) $(BIN_HIST) $(BIN_SCHED) $(BIN_RDT) $(BIN_HOLD) $(BIN_ENC) $(BIN_RING) $(BIN_ECON) $(BIN_ACK) $(BIN_LAYOUT) $(BIN_SET) $(BIN_MENU) $(BIN_CONN) $(BIN_RTC) $(BIN_SDLOG) $(BIN_SOLAR) $(BIN_CREDS) $(BIN_OTAC) $(BIN_PROF) $(BIN_ADDR) $(BIN_OQ) $(BIN_PROFB) $(BIN_PROFG) $(BIN_PROFA) $(BIN_PROFJ) $(BIN_VREG) $(BIN_NTP) $(BIN_BRANK) $(BIN_VIN) $(BIN_FUZZ) $(BIN_LFUZZ) $(BIN_BOOT) $(BIN_SWATCH)
