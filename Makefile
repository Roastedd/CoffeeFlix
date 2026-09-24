#-------------------------------------------------------------------------------
.SUFFIXES:
#-------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)

#-------------------------------------------------------------------------------
# APP_NAME sets the long name of the application
# APP_SHORTNAME sets the short name of the application
# APP_AUTHOR sets the author of the application
#-------------------------------------------------------------------------------
APP_NAME       := CoffeeFlix
APP_SHORTNAME  := CoffeeFlix
APP_AUTHOR     := roastedd, whateveritwas

# Release build by default; `make DEBUG=1` for an unoptimized build with logs
DEBUG          ?= 0

include $(DEVKITPRO)/wut/share/wut_rules

#-------------------------------------------------------------------------------
# TARGET is the name of the output
# BUILD is the directory where object files & intermediate files will be placed
# SOURCES is a list of directories containing source code
# DATA is a list of directories containing data files
# INCLUDES is a list of directories containing header files
# CONTENT is the path to the bundled folder that will be mounted as /vol/content/
# ICON is the game icon, leave blank to use default rule
# TV_SPLASH is the image displayed during bootup on the TV, leave blank to use default rule
# DRC_SPLASH is the image displayed during bootup on the DRC, leave blank to use default rule
# BOOT_SOUND is the sound that plays during bootup on DRC and TV, leave blank to use default rule
#-------------------------------------------------------------------------------

TARGET      := coffeeflix
BUILD       := build

# Find all subdirectories recursively inside src/
SOURCES     := $(shell find src -type d -not -path 'src/platform/desktop*')
DATA        :=

# Objects are named by basename, so two sources with the same file name in
# different folders would silently overwrite each other: refuse to build.
ifneq ($(BUILD),$(notdir $(CURDIR)))
DUPLICATES  := $(shell find src -name '*.cpp' -o -name '*.c' | grep -v 'src/platform/desktop' | xargs -rn1 basename | sort | uniq -d)
ifneq ($(strip $(DUPLICATES)),)
$(error Duplicate source file names (rename one of each): $(DUPLICATES))
endif
endif

# Include all source subdirectories for headers as well
INCLUDES    := $(SOURCES)

CONTENT     := content/
ICON        := branding/icon.png
TV_SPLASH   := branding/splash_tv.png
DRC_SPLASH  := branding/splash_drc.png
BOOT_SOUND  := branding/bootSound.btsnd

#-------------------------------------------------------------------------------
# options for code generation
#-------------------------------------------------------------------------------
ARCH 		:= -mcpu=750 -meabi -mhard-float

ifeq ($(DEBUG),1)
OPTFLAGS 	:= -O0 -g -DDEBUG
else
OPTFLAGS 	:= -O2 -g -ffunction-sections -fdata-sections -fomit-frame-pointer
endif

# PDF/EPUB in the reader once tools/build-deps.sh has built MuPDF.
ifneq ($(wildcard $(TOPDIR)/deps/install/lib/libmupdf.a),)
PDF_FLAGS 	:= -DHAVE_MUPDF
PDF_LIBS 	:= -lmupdf -lmupdf-third -ljpeg
endif

CFLAGS 		:= -Wall -Werror -Wno-unused-function -Wno-sign-compare $(OPTFLAGS) $(ARCH) \
			   -I$(DEVKITPRO)/portlibs/ppc/include/freetype2 \
			   $(INCLUDE) -D__WIIU__ -D__WUT__ $(PDF_FLAGS)

CXXFLAGS 	:= $(CFLAGS) -std=gnu++20

ASFLAGS 	:= -g $(ARCH)
LDFLAGS 	:= -g $(ARCH) $(RPXSPECS) -Wl,--gc-sections -Wl,-Map,$(notdir $*.map)

PKGCONF 	:= $(DEVKITPRO)/portlibs/wiiu/bin/powerpc-eabi-pkg-config
LIBS 		:= -lavformat -lavcodec -lswresample -lswscale -lavutil $(PDF_LIBS) \
			   $(shell $(PKGCONF) --libs --static SDL2_ttf SDL2_image libcurl jansson libzip) \
			   -ltinyxml2 -lgif -lbrotlidec -lbrotlicommon -lmbedtls -lmbedx509 -lmbedcrypto -lz -lwut -lm

#-------------------------------------------------------------------------------
# list of directories containing libraries, this must be the top level
# containing include and lib
#-------------------------------------------------------------------------------
LIBDIRS 	:= $(TOPDIR)/deps/install $(PORTLIBS) $(WUT_ROOT)

#-------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add additional
# rules for different file extensions
#-------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#-------------------------------------------------------------------------------

export OUTPUT := $(CURDIR)/$(TARGET)
export TOPDIR := $(CURDIR)

export VPATH := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                $(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES   := $(notdir $(foreach dir,$(SOURCES),$(wildcard $(dir)/*.c)))
CPPFILES := $(notdir $(foreach dir,$(SOURCES),$(wildcard $(dir)/*.cpp)))
SFILES   := $(notdir $(foreach dir,$(SOURCES),$(wildcard $(dir)/*.s)))
BINFILES := $(notdir $(foreach dir,$(DATA),$(wildcard $(dir)/*.*)))

#-------------------------------------------------------------------------------
# use CXX for linking C++ projects, CC for standard C
#-------------------------------------------------------------------------------
ifeq ($(strip $(CPPFILES)),)
#-------------------------------------------------------------------------------
    export LD := $(CC)
#-------------------------------------------------------------------------------
else
#-------------------------------------------------------------------------------
    export LD := $(CXX)
#-------------------------------------------------------------------------------
endif
#-------------------------------------------------------------------------------

export OFILES_BIN := $(addsuffix .o,$(BINFILES))
export OFILES_SRC := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES     := $(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN := $(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                  -I$(CURDIR)/$(BUILD)

export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

ifneq (,$(strip $(CONTENT)))
    export APP_CONTENT := $(TOPDIR)/$(CONTENT)
endif

ifneq (,$(strip $(ICON)))
    export APP_ICON := $(TOPDIR)/$(ICON)
else ifneq (,$(wildcard $(TOPDIR)/$(TARGET).png))
    export APP_ICON := $(TOPDIR)/$(TARGET).png
else ifneq (,$(wildcard $(TOPDIR)/icon.png))
    export APP_ICON := $(TOPDIR)/icon.png
endif

ifneq (,$(strip $(TV_SPLASH)))
    export APP_TV_SPLASH := $(TOPDIR)/$(TV_SPLASH)
else ifneq (,$(wildcard $(TOPDIR)/tv-splash.png))
    export APP_TV_SPLASH := $(TOPDIR)/tv-splash.png
else ifneq (,$(wildcard $(TOPDIR)/splash.png))
    export APP_TV_SPLASH := $(TOPDIR)/splash.png
endif

ifneq (,$(strip $(DRC_SPLASH)))
    export APP_DRC_SPLASH := $(TOPDIR)/$(DRC_SPLASH)
else ifneq (,$(wildcard $(TOPDIR)/drc-splash.png))
    export APP_DRC_SPLASH := $(TOPDIR)/drc-splash.png
else ifneq (,$(wildcard $(TOPDIR)/splash.png))
    export APP_DRC_SPLASH := $(TOPDIR)/splash.png
endif

ifneq (,$(strip $(BOOT_SOUND)))
    export APP_BOOT_SOUND := $(TOPDIR)/$(BOOT_SOUND)
else ifneq (,$(wildcard $(TOPDIR)/bootSound.btsnd))
    export APP_BOOT_SOUND := $(TOPDIR)/bootSound.btsnd
endif

.PHONY: $(BUILD) clean all

#-------------------------------------------------------------------------------
all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

#-------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).wuhb $(TARGET).rpx $(TARGET).elf

#-------------------------------------------------------------------------------
else
.PHONY: all

DEPENDS := $(OFILES:.o=.d)

#-------------------------------------------------------------------------------
# main targets
#-------------------------------------------------------------------------------
all : $(OUTPUT).wuhb

$(OUTPUT).wuhb : $(OUTPUT).rpx
$(OUTPUT).rpx  : $(OUTPUT).elf
$(OUTPUT).elf  : $(OFILES)

$(OFILES_SRC) : $(HFILES_BIN)

# Recompile the reader when MuPDF shows up (HAVE_MUPDF changes).
reader_screen.o : $(wildcard $(TOPDIR)/deps/install/lib/libmupdf.a)

#-------------------------------------------------------------------------------
# you need a rule like this for each extension you use as binary data
#-------------------------------------------------------------------------------
%.bin.o %_bin.h : %.bin
#-------------------------------------------------------------------------------
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

#-------------------------------------------------------------------------------
endif
#-------------------------------------------------------------------------------
