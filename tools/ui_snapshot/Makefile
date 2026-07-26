# Host build of the real firmware UI (ui.cpp + LVGL) -> PPM snapshots.
LVGL   := ../../.pio/libdeps/crowpanel_obd/lvgl
SRC    := ../../src
INC    := ../../include
CC     := cc
CXX    := c++
CFLAGS := -O1 -I. -DLV_CONF_INCLUDE_SIMPLE -I$(INC) -I$(LVGL) -I$(SRC) -w
CXXFLAGS := $(CFLAGS) -std=c++17 -DBOARD_CROWPANEL=1

LV_C   := $(shell find $(LVGL)/src -name '*.c')
LV_O   := $(patsubst $(LVGL)/src/%.c,obj/lvgl/%.o,$(LV_C))
APP_CPP:= $(SRC)/ui.cpp $(SRC)/readouts.cpp $(SRC)/vehicle_active.cpp $(SRC)/vehicle_registry.cpp \
          $(SRC)/vehicles/gm_sierra_lz0.cpp $(SRC)/vehicles/generic_obd.cpp $(SRC)/vehicles/bmw_f10_535i.cpp $(SRC)/vehicles/audi_q5.cpp \
          $(SRC)/gauge_model.cpp $(SRC)/nav_model.cpp \
          $(SRC)/pid_decode.cpp $(SRC)/settings_menu.cpp $(SRC)/rtc.cpp $(SRC)/history.cpp $(SRC)/settings.cpp $(SRC)/alarm_holdoff.cpp $(SRC)/solar.cpp main.cpp
APP_O  := $(patsubst %.cpp,obj/app/%.o,$(notdir $(APP_CPP)))

snapshot: $(LV_O) $(APP_O)
	$(CXX) -o $@ $(LV_O) $(APP_O) -lm

obj/lvgl/%.o: $(LVGL)/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

obj/app/%.o:
	@mkdir -p obj/app
	$(CXX) $(CXXFLAGS) -c $(filter %/$*.cpp %$*.cpp,$(APP_CPP)) -o $@

clean:
	rm -rf obj snapshot *.ppm
