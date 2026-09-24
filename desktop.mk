# Desktop preview build (Linux): make -f desktop.mk
# Needs: SDL2, SDL2_ttf, SDL2_image, libcurl, jansson, tinyxml2, libzip, mbedtls
# and `tools/build-deps.sh --host` for FFmpeg, libsmb2 and MuPDF (PDF/EPUB).
BUILD    := build-desktop
TARGET   := $(BUILD)/coffeeflix
PREFIX   := deps/host

SRCS := $(shell find src -name '*.cpp' -not -path 'src/platform/wiiu/*' -not -path 'src/vendor/*')
OBJS := $(SRCS:%.cpp=$(BUILD)/%.o)
DEPS := $(OBJS:.o=.d)

PKGS     := sdl2 SDL2_ttf SDL2_image libcurl jansson tinyxml2 libzip
ifneq ($(wildcard $(PREFIX)/lib/libmupdf.a),)
PDF_FLAGS := -DHAVE_MUPDF
PDF_LIBS  := -lmupdf -lmupdf-third
PKGS      += harfbuzz freetype2 libjpeg
endif

CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=gnu++20 -Wall -Wno-sign-compare -Wno-unused-function -MMD -MP \
            -Isrc -I$(PREFIX)/include $(PDF_FLAGS) $(shell pkg-config --cflags $(PKGS))
LDLIBS   := -L$(PREFIX)/lib -lavformat -lavcodec -lswresample -lswscale -lavutil -lsmb2 $(PDF_LIBS) \
            $(shell pkg-config --libs $(PKGS)) -lmbedtls -lmbedx509 -lmbedcrypto -lz -lpthread -lm

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) -o $@ $^ $(LDLIBS)

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Recompile the reader when MuPDF shows up (HAVE_MUPDF changes).
$(BUILD)/src/screens/reader_screen.o: $(wildcard $(PREFIX)/lib/libmupdf.a)

clean:
	rm -rf $(BUILD)

-include $(DEPS)
.PHONY: all clean
