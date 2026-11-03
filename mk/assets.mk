# ---- assets -----------------------------------------------------------------
# OS artwork and fonts for romfs, generated on macOS by tools/mkassets.swift
# and checked in under assets/gen.

ASSET_FILES := $(wildcard assets/gen/*)
# The browser's bundled pages, romfs "web/", and its trusted roots, "ssl/"
# (tools/mkroots.py).
WEB_FILES := $(wildcard assets/web/*)
SSL_FILES := $(wildcard assets/ssl/*)
# System sounds, romfs "sounds/", synthesized by tools/mksounds.py and checked in.
SOUND_FILES := $(wildcard assets/sounds/*)

# Local Marvell firmware (never checked in). See docs/hardware/WL54CF.md.
WL54CF ?= 0
WL54CF_FW_DIR ?= firmware/wl54cf
WL54CF_FW :=
ifeq ($(WL54CF),1)
WL54CF_FW := $(WL54CF_FW_DIR)/cf8385_helper.bin $(WL54CF_FW_DIR)/cf8385.bin
endif

# Optional card drivers are independent relocatable kernel images. Only the
# CIS-selected image is read into RAM; the dispatcher stays in the kernel.
DRIVER_NAMES := ne2000 xi825 wl54cf btuart
DRIVER_FILES := $(foreach d,$(DRIVER_NAMES),$(OUT)/drivers/$(d).ko)
$(OUT)/kmod/%.o: kernel/modules/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_COMMON) -O2 -Ikernel -MMD -c -o $@ $<
$(OUT)/drivers/%.ko: $(OUT)/kmod/%.o $(OUT)/kernel/drivers/%.o
	@mkdir -p $(dir $@)
	$(LD) -m elf32ltsmip -r -o $@ $^
	$(OBJCOPY) --strip-debug --remove-section=.reginfo --remove-section=.MIPS.abiflags \
	    --remove-section=.pdr --remove-section=.gnu.attributes $@

$(OUT)/services.bin: config/services.json tools/mkservices.py
	@mkdir -p $(dir $@)
	$(PYTHON) tools/mkservices.py $< $@
