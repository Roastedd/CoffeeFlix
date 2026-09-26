# Desktop preview build (macOS / Linux): make -f desktop.mk run
#
#   macOS:  brew install pkg-config cmake sdl2 sdl2_ttf sdl2_image ffmpeg curl jansson tinyxml2 libzip
#           tools/build-deps.sh --host libsmb2
#   Linux:  the same libraries from your package manager, then tools/build-deps.sh --host
#
# deps/host (tools/build-deps.sh --host) provides libsmb2 and, optionally, FFmpeg.
# Without FFmpeg there, the system's is used through pkg-config.
BUILD    := build-desktop
TARGET   := $(BUILD)/coffeeflix
PREFIX   := deps/host

SRCS := $(shell find src -name '*.cpp' -not -path 'src/platform/wiiu/*')
OBJS := $(SRCS:%.cpp=$(BUILD)/%.o)
DEPS := $(OBJS:.o=.d)

PKGS       := sdl2 SDL2_ttf SDL2_image libcurl jansson tinyxml2 libzip libcrypto
PKG_CONFIG := pkg-config
LIBDIRS    :=

ifeq ($(shell uname -s),Darwin)
BREW := $(shell brew --prefix 2>/dev/null)
ifneq ($(BREW),)
# Homebrew's curl and OpenSSL are keg-only, so pkg-config doesn't find them on its own
PKG_CONFIG := PKG_CONFIG_PATH="$(BREW)/opt/curl/lib/pkgconfig:$(BREW)/opt/openssl@3/lib/pkgconfig:$$PKG_CONFIG_PATH" pkg-config
LIBDIRS    := -L$(BREW)/lib
endif
endif

ifneq ($(wildcard $(PREFIX)/lib/libavcodec.a),)
FFMPEG_LIBS   := -lavformat -lavcodec -lswresample -lswscale -lavutil $(LIBDIRS) -lmbedtls -lmbedx509 -lmbedcrypto
else
FFMPEG_PKGS   := libavformat libavcodec libswresample libswscale libavutil
FFMPEG_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(FFMPEG_PKGS))
FFMPEG_LIBS   := $(shell $(PKG_CONFIG) --libs $(FFMPEG_PKGS))
endif

# The version the app reports, as in the Makefile.
APP_VERSION ?= $(shell git describe --tags --always 2>/dev/null | sed 's/^v//')
$(shell mkdir -p $(BUILD) && printf '#define APP_VERSION "%s"\n' '$(or $(APP_VERSION),dev)' > $(BUILD)/app_version.h.new \
	&& (cmp -s $(BUILD)/app_version.h.new $(BUILD)/app_version.h || cp $(BUILD)/app_version.h.new $(BUILD)/app_version.h); \
	rm -f $(BUILD)/app_version.h.new)

CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=gnu++20 -Wall -Wno-sign-compare -Wno-unused-function -MMD -MP \
            -Isrc -I$(BUILD) -I$(PREFIX)/include $(FFMPEG_CFLAGS) $(shell $(PKG_CONFIG) --cflags $(PKGS))
LDLIBS   := -L$(PREFIX)/lib -lsmb2 $(FFMPEG_LIBS) \
            $(shell $(PKG_CONFIG) --libs $(PKGS)) -lz -lpthread -lm

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) -o $@ $^ $(LDLIBS)

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Runs from the repo root so content/ is found; settings and media live in data/
run: $(TARGET)
	./$(TARGET)

clean:
	rm -rf $(BUILD)

-include $(DEPS)
.PHONY: all run clean
