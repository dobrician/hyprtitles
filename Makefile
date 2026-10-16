# --no-gnu-unique lets Hyprland unload and reload the plugin with g++
ifeq ($(CXX),g++)
    EXTRA_FLAGS = --no-gnu-unique
else
    EXTRA_FLAGS =
endif

CXXFLAGS ?= -O2
CXXFLAGS += -shared -fPIC -std=c++2b -Wno-c++11-narrowing
INCLUDES = `pkg-config --cflags pixman-1 libdrm hyprland libinput libudev wayland-server xkbcommon`

SRC = $(wildcard src/*.cpp)
HDR = $(wildcard src/*.hpp)
TARGET = hyprtitles.so

all: $(TARGET)

$(TARGET): $(SRC) $(HDR)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(EXTRA_FLAGS) $(INCLUDES) $(SRC) -o $@

test:
	lua tests/test_examples.lua

clean:
	rm -f ./$(TARGET)

.PHONY: all test clean
