
# OTA update (docs/os/OTA.md) of the built kernel and romfs as VERSION, signed
# with GLAZE_OTA_KEY (default ~/.config/glaze/ota-ed25519.pem): out/ota/os/.
# It builds nothing, so run it natively after ./build.sh: signing needs
# OpenSSL 3 and the private key, which the container has neither of.
OTA_NOTES ?=
ota:
	$(PYTHON) tools/mkota.py build --kernel $(OUT)/kernel.elf --romfs $(OUT)/romfs.img \
	    --version '$(GLAZE_VERSION)' --build '$(BUILD_ID)' --notes '$(OTA_NOTES)' --out $(OUT)/ota
.PHONY: ota

# Portable static store: run ./build.sh store, then publish out/store over HTTPS.
# The emulator packages come from a built sibling glaze-emulators checkout.
EMU_DIR ?= ../glaze-emulators
store: packages
	$(PYTHON) tools/mkstore.py --packages $(OUT)/pkg --packages $(EMU_DIR)/out/pkg --output $(OUT)/store
.PHONY: store

$(OUT)/include/js_bootstrap.h: apps/browser/js_dom.js apps/browser/js_web.js tools/mkjsbootstrap.py
	python3 tools/mkjsbootstrap.py $< $@
$(OUT)/apps/browser/script.o $(OUT)/dyn/apps/browser/script.o: $(OUT)/include/js_bootstrap.h

# Standalone ROM/CF recovery: boots even with blank NAND, root stays in RAM.
$(OUT)/recovery/consts.flags: $(GB_DIR)/kernel.bin $(GB_DIR)/romfs.img tools/mkrecovery.py
	@mkdir -p $(dir $@)
	$(PYTHON) tools/mkrecovery.py --kernel $(GB_DIR)/kernel.bin --romfs $(GB_DIR)/romfs.img --flags $@
$(OUT)/recovery/start.o: boot/recovery_start.S $(OUT)/recovery/consts.flags
	$(CC) $(SPL_CFLAGS) $$(cat $(OUT)/recovery/consts.flags) -c -o $@ $<
$(OUT)/recovery/start.elf: $(OUT)/recovery/start.o boot/recovery.lds
	$(LD) -m elf32ltsmip -T boot/recovery.lds -o $@ $<
$(OUT)/recovery/start.bin: $(OUT)/recovery/start.elf
	$(OBJCOPY) -O binary $< $@
$(OUT)/recovery/RECOVER.BIN: $(OUT)/recovery/start.bin $(GB_DIR)/kernel.bin $(GB_DIR)/romfs.img tools/mkrecovery.py
	$(PYTHON) tools/mkrecovery.py --kernel $(GB_DIR)/kernel.bin --romfs $(GB_DIR)/romfs.img --stub $< --out $@
recovery: $(OUT)/recovery/RECOVER.BIN
.PHONY: recovery

$(OUT)/recovery/REPAIR.BIN: $(OUT)/recovery/start.bin $(GB_DIR)/kernel.bin $(GB_DIR)/romfs.img tools/mkrecovery.py
	$(PYTHON) tools/mkrecovery.py --kernel $(GB_DIR)/kernel.bin --romfs $(GB_DIR)/romfs.img --stub $< --nand-repair --out $@
recovery-repair: $(OUT)/recovery/REPAIR.BIN
.PHONY: recovery-repair
