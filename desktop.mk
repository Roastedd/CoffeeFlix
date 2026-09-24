# Desktop preview build (Linux): make -f desktop.mk
# Needs: SDL2, SDL2_ttf, SDL2_image, libcurl, jansson, tinyxml2, libzip, mbedtls
# and `tools/build-deps.sh --host` for FFmpeg.
BUILD    := build-desktop
TARGET   := $(BUILD)/coffeeflix
FFMPEG   := deps/host

SRCS := $(shell find src -name '*.cpp' -not -path 'src/platform/wiiu/*' -not -path 'src/vendor/*')
OBJS := $(SRCS:%.cpp=$(BUILD)/%.o)
DEPS := $(OBJS:.o=.d)

PKGS     := sdl2 SDL2_ttf SDL2_image libcurl jansson tinyxml2 libzip
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=gnu++20 -Wall -Wno-sign-compare -Wno-unused-function -MMD -MP \
            -Isrc -I$(FFMPEG)/include $(shell pkg-config --cflags $(PKGS))
LDLIBS   := -L$(FFMPEG)/lib -lavformat -lavcodec -lswresample -lswscale -lavutil \
            $(shell pkg-config --libs $(PKGS)) -lmbedtls -lmbedx509 -lmbedcrypto -lz -lpthread -lm

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) -o $@ $^ $(LDLIBS)

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD)

-include $(DEPS)
.PHONY: all clean
