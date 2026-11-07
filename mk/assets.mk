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

# Marvell firmware for the WL54-CF, the BE-300's most usable networking: in
# every build by default (docs/hardware/WL54CF.md). The binaries are never
# checked in: fetch them with tools/fetch_wl54cf_firmware.py. Its licence
# and provenance go into romfs beside them, since every image or OTA that
# carries the firmware redistributes it. WL54CF=0 leaves it out, as the
# public emulator image must (the licence covers Marvell silicon only).
WL54CF ?= 1
WL54CF_FW_DIR ?= firmware/wl54cf
WL54CF_FW :=
WL54CF_NOTICES :=
ifeq ($(WL54CF),1)
WL54CF_FW := $(WL54CF_FW_DIR)/cf8385_helper.bin $(WL54CF_FW_DIR)/cf8385.bin
WL54CF_NOTICES := firmware/LICENCE.Marvell firmware/wl54cf-linux.json
ifneq ($(words $(wildcard $(WL54CF_FW))),2)
$(error WL54-CF firmware is missing from $(WL54CF_FW_DIR): run python3 tools/fetch_wl54cf_firmware.py, or build with WL54CF=0)
endif
endif

# Optional card drivers are independent relocatable kernel images. Only the
# CIS-selected image is read into RAM; the dispatcher stays in the kernel.
DRIVER_NAMES := ne2000 xi825 wl54cf cfuart
# PC Cards in the PCMCIA jacket (docs/hardware/PCCARDS.md): the 3c589 and
# Hermes drivers. The NAND romfs has no room for them beside the default
# apps, so, like MODEM (mk/libs.mk), GlazeBoot builds default to them and
# NAND images need PCCARDS=1 NAND_DROP=wince. Without them a jacket card is
# identified and its socket reserved.
PCCARDS ?= $(if $(filter glazeboot,$(MAKECMDGOALS)),1,0)
# Never linked into the kernel (mk/kernel.mk), built in or not.
PCCARD_DRIVERS := 3c589 hermes
ifeq ($(PCCARDS),1)
DRIVER_NAMES += $(PCCARD_DRIVERS)
endif
DRIVER_FILES := $(foreach d,$(DRIVER_NAMES),$(OUT)/drivers/$(d).ko)
$(OUT)/kmod/%.o: kernel/modules/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_COMMON) -O2 -Ikernel -MMD -c -o $@ $<
$(OUT)/drivers/%.ko: $(OUT)/kmod/%.o $(OUT)/kernel/drivers/%.o
	@mkdir -p $(dir $@)
	$(LD) -m elf32ltsmip -r -o $@ $^
	$(OBJCOPY) --strip-debug --remove-section=.reginfo --remove-section=.MIPS.abiflags \
	    --remove-section=.pdr --remove-section=.gnu.attributes --remove-section=.comment \
	    --strip-unneeded --keep-symbol=glaze_driver $@

$(OUT)/services.bin: config/services.json tools/mkservices.py
	@mkdir -p $(dir $@)
	$(PYTHON) tools/mkservices.py $< $@
