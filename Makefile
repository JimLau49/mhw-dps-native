# MHW DPS Meter (native) — Stracker's Loader plugin for Monster Hunter: World.
# Cross-compiles a Windows x64 DLL with mingw-w64 (brew install mingw-w64).

VERSION     := 0.1.0
BUILD_STAMP := $(shell date -u +"%Y-%m-%d %H:%MZ")

TOOLCHAIN ?= x86_64-w64-mingw32-
CXX       := $(TOOLCHAIN)g++
CC        := $(TOOLCHAIN)gcc
OBJCOPY   := $(TOOLCHAIN)objcopy
STRIP     := $(TOOLCHAIN)strip

BUILD := build
DIST  := dist

DEFINES := -DNDEBUG -DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_WIN32_WINNT=0x0A00 -DUNICODE -D_UNICODE \
           -DIMGUI_IMPL_WIN32_DISABLE_GAMEPAD -DIMGUI_DISABLE_OBSOLETE_FUNCTIONS \
           -DIMGUI_USER_CONFIG='"imconfig_dps.h"'
INCLUDES := -Isrc -Ithird_party -Ithird_party/imgui -Ithird_party/imgui/backends -Ithird_party/minhook/include

OPT      ?= -O2
WARN     := -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-cast-function-type
COMMON   := $(OPT) -g -fno-strict-aliasing -ffunction-sections -fdata-sections $(DEFINES) $(INCLUDES)
CXXFLAGS := $(COMMON) -std=c++20 $(WARN) -DDPS_VERSION='"$(VERSION)"' -DDPS_BUILD_STAMP='"$(BUILD_STAMP)"'
CFLAGS   := $(COMMON) -std=c11 -w
# Third-party code: no warnings, and ImGui's DX11 backend gets D3DCompile routed
# through our runtime loader (the bottle's d3dcompiler_47 is loaded on first use).
TP_CXXFLAGS := $(COMMON) -std=c++20 -w

LDFLAGS := -shared -static -static-libgcc -static-libstdc++ -Wl,--gc-sections \
           -Wl,--exclude-all-symbols -Wl,--dynamicbase -Wl,--nxcompat -Wl,--high-entropy-va
LIBS    := -luser32 -lgdi32 -ldwmapi -limm32 -lole32 -luuid -ldxguid

# --- sources -----------------------------------------------------------------

CORE_SRC := \
  src/common.cpp \
  src/log.cpp \
  src/memory.cpp \
  src/address_map.cpp \
  src/game_build.cpp \
  src/game_data.cpp \
  src/settings.cpp \
  src/game/game_reader.cpp \
  src/hooks/hook_manager.cpp \
  src/hooks/game_hooks.cpp \
  src/meter/hit_tracker.cpp \
  src/meter/monster_tracker.cpp \
  src/meter/cart_tracker.cpp \
  src/meter/recorder.cpp \
  src/meter/fight_log_store.cpp \
  src/meter/time_trial.cpp \
  src/meter/meter.cpp \
  src/render/d3d11_hook.cpp \
  src/render/d3dcompiler_loader.cpp \
  src/render/overlay.cpp \
  src/render/input.cpp

PLUGIN_SRC := src/dllmain.cpp

IMGUI_SRC := \
  third_party/imgui/imgui.cpp \
  third_party/imgui/imgui_draw.cpp \
  third_party/imgui/imgui_tables.cpp \
  third_party/imgui/imgui_widgets.cpp \
  third_party/imgui/backends/imgui_impl_win32.cpp

IMGUI_DX11_SRC := third_party/imgui/backends/imgui_impl_dx11.cpp

MINHOOK_SRC := \
  third_party/minhook/src/buffer.c \
  third_party/minhook/src/hook.c \
  third_party/minhook/src/trampoline.c \
  third_party/minhook/src/hde/hde64.c

TEST_SRC := tests/harness.cpp

CORE_OBJ    := $(CORE_SRC:%.cpp=$(BUILD)/%.o)
PLUGIN_OBJ  := $(PLUGIN_SRC:%.cpp=$(BUILD)/%.o)
IMGUI_OBJ   := $(IMGUI_SRC:%.cpp=$(BUILD)/%.o) $(IMGUI_DX11_SRC:%.cpp=$(BUILD)/%.o)
MINHOOK_OBJ := $(MINHOOK_SRC:%.c=$(BUILD)/%.o)
TEST_OBJ    := $(TEST_SRC:%.cpp=$(BUILD)/%.o)
LIB_OBJ     := $(CORE_OBJ) $(IMGUI_OBJ) $(MINHOOK_OBJ)

DLL        := $(BUILD)/MhwDpsMeter.dll
HARNESS    := $(BUILD)/dps-harness.exe
PACKAGE    := $(DIST)/MhwDpsMeter-native-$(VERSION).zip

.PHONY: all plugin harness package clean data

# Flag changes in this file must rebuild everything (make does not track flags).
$(LIB_OBJ) $(PLUGIN_OBJ) $(TEST_OBJ): Makefile

all: plugin harness

plugin: $(DLL)

harness: $(HARNESS)

$(DLL): $(LIB_OBJ) $(PLUGIN_OBJ)
	$(CXX) -o $@ $^ $(LDFLAGS) $(LIBS)
	$(OBJCOPY) --only-keep-debug $@ $@.debug
	$(STRIP) --strip-debug $@
	$(OBJCOPY) --add-gnu-debuglink=$@.debug $@

$(HARNESS): $(LIB_OBJ) $(TEST_OBJ)
	$(CXX) -o $@ $^ -static -static-libgcc -static-libstdc++ -Wl,--gc-sections $(LIBS)

$(BUILD)/third_party/imgui/backends/imgui_impl_dx11.o: third_party/imgui/backends/imgui_impl_dx11.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(TP_CXXFLAGS) -DD3DCompile=DpsD3DCompile -MMD -MP -c $< -o $@

$(BUILD)/third_party/%.o: third_party/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(TP_CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/third_party/%.o: third_party/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

# Regenerate src/game_data_tables.inc from the C# project's data files.
CS_DATA ?= ../mhw-dps-meter/data
data:
	python3 tools/gen_tables.py $(CS_DATA) src/game_data_tables.inc

package: $(DLL)
	@rm -rf $(BUILD)/package && mkdir -p $(BUILD)/package/nativePC/plugins/MhwDpsMeter $(DIST)
	cp $(DLL) $(BUILD)/package/nativePC/plugins/
	cp -R addresses $(BUILD)/package/nativePC/plugins/MhwDpsMeter/Addresses
	cd $(BUILD)/package && rm -f ../../$(PACKAGE) && zip -qr ../../$(PACKAGE) nativePC
	@echo "wrote $(PACKAGE)"

clean:
	rm -rf $(BUILD) $(DIST)

-include $(LIB_OBJ:.o=.d) $(PLUGIN_OBJ:.o=.d) $(TEST_OBJ:.o=.d)
