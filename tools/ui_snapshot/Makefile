# Host build of the real firmware UI (ui.cpp + LVGL) -> PPM snapshots.
LVGL   := ../../.pio/libdeps/crowpanel_obd/lvgl
SRC    := ../../src
INC    := ../../include
CC     := cc
CXX    := c++
CFLAGS := -O1 -I. -DLV_CONF_INCLUDE_SIMPLE -I$(INC) -I$(LVGL) -I$(SRC) -w
# FW_DATE is pinned, NOT __DATE__. ui.cpp prints it on the splash screen, so an
# unpinned build re-renders splash_day.png with today's date on every run — a
# spurious binary diff on an image nothing in the change actually touched, and
# it makes byte-comparing the committed renders impossible. The value is a
# label for a doc screenshot, so a fixed string is the honest thing to show.
CXXFLAGS := $(CFLAGS) -std=c++17 -DBOARD_CROWPANEL=1 '-DFW_DATE="(docs)"'

LV_C   := $(shell find $(LVGL)/src -name '*.c')
LV_O   := $(patsubst $(LVGL)/src/%.c,obj/lvgl/%.o,$(LV_C))
# Vehicle profiles are globbed, not listed: vehicle_registry.cpp references every
# profile symbol, so a profile added to src/vehicles/ but missed here breaks the
# link with an undefined reference. That is exactly what happened when the Jeep
# WS profile landed — CI never caught it because CI does not build this tool.
APP_CPP:= $(SRC)/ui.cpp $(SRC)/readouts.cpp $(SRC)/vehicle_active.cpp $(SRC)/vehicle_registry.cpp \
          $(wildcard $(SRC)/vehicles/*.cpp) \
          $(SRC)/gauge_model.cpp $(SRC)/nav_model.cpp \
          $(SRC)/pid_decode.cpp $(SRC)/settings_menu.cpp $(SRC)/rtc.cpp $(SRC)/history.cpp $(SRC)/settings.cpp $(SRC)/alarm_holdoff.cpp $(SRC)/solar.cpp main.cpp
APP_O  := $(patsubst %.cpp,obj/app/%.o,$(notdir $(APP_CPP)))

snapshot: $(LV_O) $(APP_O)
	$(CXX) -o $@ $(LV_O) $(APP_O) -lm

obj/lvgl/%.o: $(LVGL)/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Every header the app objects can include. Listed wholesale rather than tracked
# per-file with -MMD: 18 translation units rebuild in seconds, and a snapshot
# tool that renders the README images must never be subtly out of date.
APP_H  := $(wildcard $(SRC)/*.h) $(wildcard $(SRC)/vehicles/*.h) $(wildcard $(INC)/*.h) Arduino.h

# App objects land in a flat obj/app/ but their sources are spread across $(SRC)
# and $(SRC)/vehicles, so the prerequisite is recovered from APP_CPP by name via
# secondary expansion. This rule previously declared NO prerequisites at all:
# once obj/app was populated, make answered "'snapshot' is up to date" after any
# src/ edit and ./snapshot re-emitted the OLD pixels — a silent way to commit
# stale README renders. Keep the prerequisites.
.SECONDEXPANSION:
obj/app/%.o: $$(filter %/$$*.cpp %$$*.cpp,$(APP_CPP)) $(APP_H)
	@mkdir -p obj/app
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -rf obj snapshot *.ppm
