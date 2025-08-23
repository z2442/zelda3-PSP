# PSP Development Makefile for Zelda3
TARGET := zelda3
ROM := tables/zelda3.sfc
SRCS := $(wildcard src/*.c snes/*.c) third_party/gl_core/gl_core_3_1.c third_party/opus-1.3.1-stripped/opus_decoder_amalgam.c
OBJS := $(SRCS:%.c=%.o)
PYTHON := /usr/bin/env python3
CFLAGS := $(if $(CFLAGS),$(CFLAGS),-g -O2 -Werror) -I .
CFLAGS += #$(shell psp-config --cflags) -DSYSTEM_VOLUME_MIXER_AVAILABLE=0
LDFLAGS := #$(shell psp-config --ldflags)

# PSP-specific modules and libraries
LIBS = -lSDL2 -lGL -lz -lpspvfpu -lpspfpu -lpsphprm -lpspsdk -lpspctrl -lpspumd -lpsprtc \
       -lpsppower -lpspgum -lpspgu -lpspge -lpspaudiolib -lpspaudio -lpsphttp -lpspssl -lpspwlan \
	   -lpspnet_adhocmatching -lpspnet_adhoc -lpspnet_adhocctl -lm -lpspvram -lpspdisplay

# Build PRX when requested; default to EBOOT
BUILD_PRX ?= 1
EXTRA_TARGETS := EBOOT.PBP
PSP_EBOOT_TITLE = zelda3


PSPSDK=$(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak

.PHONY: all clean clean_obj clean_gen

# All target: builds the executable and assets
all: $(TARGET).prx zelda3_assets.dat $(EXTRA_TARGETS)
$(TARGET_PBP): $(TARGET_EXEC)
TARGET_PBP := EBOOT.PBP

# Extract game resources using the Python script
zelda3_assets.dat:
	@echo "Extracting game resources"
	$(PYTHON) assets/restool.py --extract-from-rom

# Clean the build
clean: clean_obj clean_gen
clean_obj:
	@$(RM) $(OBJS) $(TARGET).elf $(TARGET).prx EBOOT.PBP PARAM.SFO
clean_gen:
	@$(RM) zelda3_assets.dat tables/zelda3_assets.dat tables/*.txt tables/*.png tables/sprites/*.png tables/*.yaml
	@rm -rf tables/__pycache__ tables/dungeon tables/img tables/overworld tables/sound

.PHONY: prx
prx:
	@$(MAKE) BUILD_PRX=1