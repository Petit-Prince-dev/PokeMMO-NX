.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

# The application, as the console shows it (stored in the .nacp file; the icon is a 256x256 JPEG).
TARGET      := PokeMMO
APP_TITLE   := PokeMMO
APP_AUTHOR  := Petit_Prince
APP_VERSION := 1.1.0
ICON        := icon.jpg

BUILD    := build
SOURCES  := source
INCLUDES := include

ARCH    := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS  := -g -Wall -Wextra -Werror -O2 -mno-outline-atomics -ffunction-sections $(ARCH)
CFLAGS  += $(INCLUDE) -D__SWITCH__ -DAPP_VERSION=\"$(APP_VERSION)\"
ASFLAGS := -g $(ARCH)
LDFLAGS  = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)
# display.c replaces two libnx calls (see there)
LDFLAGS += -Wl,--wrap=framebufferCreate,--wrap=nwindowDequeueBuffer

# Mesa's EGL is C++, hence libstdc++
LIBS    := -lglad -lEGL -lglapi -ldrm_nouveau -lz -lstdc++ -lm -lnx -lm
LIBDIRS := $(PORTLIBS) $(LIBNX)

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT  := $(CURDIR)/$(TARGET)
export TOPDIR  := $(CURDIR)
export VPATH   := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
SFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))

export LD       := $(CC)
export OFILES   := $(CFILES:.c=.o) $(SFILES:.s=.o)
export INCLUDE  := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) $(foreach dir,$(LIBDIRS),-I$(dir)/include)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export NROFLAGS += --icon=$(TOPDIR)/$(ICON) --nacp=$(CURDIR)/$(TARGET).nacp

.PHONY: $(BUILD) clean all

all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf

else

.PHONY: all

DEPENDS := $(OFILES:.o=.d)

all: $(OUTPUT).nro

$(OUTPUT).nro: $(OUTPUT).elf $(OUTPUT).nacp

$(OUTPUT).elf: $(OFILES)

-include $(DEPENDS)

endif
