# Include this after setting SOURCES (and optionally LDLIBS / CPPFLAGS).
GLAZE_SDK := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
ifneq ($(words $(GLAZE_SDK) $(CURDIR)),2)
$(error GNU make requires SDK and project paths without whitespace)
endif
CROSS ?= mipsel-linux-gnu-
PYTHON ?= python3
BUILD ?= build
SOURCES ?= $(wildcard *.c)
SDK_CC := $(CROSS)gcc
SDK_LD := $(CROSS)ld
SDK_OBJCOPY := $(CROSS)objcopy
SDK_INCLUDE := $(GLAZE_SDK)/sysroot/usr/include
SDK_LIB := $(GLAZE_SDK)/sysroot/usr/lib
SDK_TOOL := $(GLAZE_SDK)/bin/glaze-sdk
SDK_ARCH := -march=mips2 -mabi=32 -msoft-float -G0 -mabicalls -fPIC
SDK_CFLAGS := $(SDK_ARCH) -std=gnu11 -ffreestanding -fno-builtin -nostdlib \
    -fno-stack-protector -O2 -g -Wall -Wextra -ffunction-sections -fdata-sections \
    -nostdinc -isystem $(SDK_INCLUDE) -isystem $(shell $(SDK_CC) -print-file-name=include)
SDK_LDFLAGS := -m elf32ltsmip --hash-style=sysv -z now -z relro -z text \
    -z max-page-size=4096 -z separate-code -pie -e _start --export-dynamic \
    --gc-sections --dynamic-linker /lib/ld-glaze.so.1
SDK_OBJECTS := $(patsubst %.c,$(BUILD)/%.o,$(SOURCES))
SDK_START := $(SDK_LIB)/crt0.o $(SDK_LIB)/start.o
SDK_LIBS := $(wildcard $(SDK_LIB)/*.so.1)
SDK_RESOURCES := $(shell find res -type f 2>/dev/null)
GLAZE_USB ?= $(GLAZE_SDK)/bin/glaze-usb
BE300 ?= be300

.DEFAULT_GOAL := all
.PHONY: all package check clean cf run deploy
all: package
$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(SDK_CC) $(SDK_CFLAGS) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

# Retain symbols here; only the packaged copy is stripped.
$(BUILD)/app.elf: $(SDK_OBJECTS) $(SDK_START) $(SDK_LIBS) Makefile $(GLAZE_SDK)/share/app.mk
	$(SDK_LD) $(SDK_LDFLAGS) $(LDFLAGS) -o $@ $(SDK_START) $(SDK_OBJECTS) \
	    -L$(SDK_LIB) --as-needed $(LDLIBS) -l:libui.so.1 -l:libgfx.so.1 -l:libc.so.1
$(BUILD)/app: $(BUILD)/app.elf
	$(SDK_OBJCOPY) --strip-all $< $@
package: $(BUILD)/app Info $(SDK_RESOURCES)
	$(PYTHON) $(SDK_TOOL) package --project . --elf $(BUILD)/app --output $(BUILD)/app.gzp
check: package
	$(PYTHON) $(SDK_TOOL) check $(BUILD)/app.gzp
cf: package
	sh $(GLAZE_SDK)/tools/mkcfimg.sh $(BUILD)/card.img 16 16 $(BUILD)/app.gzp
run: cf
	@test -n "$(GLAZE_NAND)" || { echo 'Set GLAZE_NAND to a matching Glaze NAND image' >&2; exit 1; }
	cp "$(GLAZE_NAND)" $(BUILD)/run-nand.img
	$(BE300) --nand $(BUILD)/run-nand.img --cf $(BUILD)/card.img --rtc-host-time
deploy: check
	$(GLAZE_USB) put $(BUILD)/app.gzp /Downloads/sdk-app.gzp
	$(GLAZE_USB) run /Downloads/sdk-app.gzp
clean:
	rm -f $(SDK_OBJECTS) $(SDK_OBJECTS:.o=.d) $(BUILD)/app.elf $(BUILD)/app $(BUILD)/app.gzp
-include $(SDK_OBJECTS:.o=.d)
