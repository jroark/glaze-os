# ---- hardware-support packages ------------------------------------------------
# Drivers, firmware and service variants that don't fit in the NAND romfs,
# installed like apps (glaze/hwpkg.h, docs/os/HARDWARE_PACKAGES.md):
#
#   hw-pccards   drivers/3c589.ko, drivers/hermes.ko   (what PCCARDS=1 builds in)
#   hw-modem     bin/net with lwIP's PPP               (what MODEM=1 builds in)
#   hw-wl54cf    firmware/cf8385*.bin                  (what WL54CF=1 builds in)
#
# Two steps, as for `make ota`: `make hwpkg-files` builds the payloads (in the
# container too), then `make hwpkgs` signs and packs them natively, with
# OpenSSL 3 and GLAZE_OTA_KEY. The code only loads on this VERSION. The
# firmware package is unsigned and built only from local firmware/wl54cf
# files, which are vendor material: never commit or distribute it.

GLAZE_OTA_KEY ?= $(HOME)/.config/glaze/ota-ed25519.pem
HWPKG_VERSION ?= $(GLAZE_VERSION)
HWPKG_MODEM_OUT := $(OUT)/modem
HWPKG_DRIVERS := $(foreach d,$(PCCARD_DRIVERS),$(OUT)/drivers/$(d).ko)

# bin/net with PPP changes lwIP's structures: its own output directory.
$(HWPKG_MODEM_OUT)/romfs/bin/net: FORCE
	$(MAKE) OUT=$(HWPKG_MODEM_OUT) MODEM=1 BUILD_ID='$(BUILD_ID)' $@

hwpkg-files: $(HWPKG_DRIVERS) $(HWPKG_MODEM_OUT)/romfs/bin/net

HWPKG = $(PYTHON) tools/mkhwpkg.py --version '$(HWPKG_VERSION)' --os '$(GLAZE_VERSION)'
hwpkgs:
	@for f in $(HWPKG_DRIVERS) $(HWPKG_MODEM_OUT)/romfs/bin/net; do \
	    [ -f $$f ] || { echo "$$f is missing: run 'make hwpkg-files' (./build.sh hwpkg-files) first" >&2; exit 1; }; done
	$(HWPKG) --key '$(GLAZE_OTA_KEY)' --id hw-pccards --name 'PC Card Drivers' --out $(OUT)/pkg/hw-pccards.gzp \
	    $(foreach d,$(PCCARD_DRIVERS),drivers/$(d).ko=$(OUT)/drivers/$(d).ko)
	$(HWPKG) --key '$(GLAZE_OTA_KEY)' --id hw-modem --name 'Dial-up Modem' --out $(OUT)/pkg/hw-modem.gzp \
	    bin/net=$(HWPKG_MODEM_OUT)/romfs/bin/net
	@if [ -f $(WL54CF_FW_DIR)/cf8385_helper.bin ] && [ -f $(WL54CF_FW_DIR)/cf8385.bin ]; then \
	    $(MAKE) --no-print-directory hwpkg-wl54cf; \
	else echo "[mkhwpkg] no $(WL54CF_FW_DIR) firmware: skipping hw-wl54cf"; fi

hwpkg-wl54cf:
	$(HWPKG) --unsigned --id hw-wl54cf --name 'WL54-CF Firmware' --out $(OUT)/pkg/hw-wl54cf.gzp \
	    firmware/cf8385_helper.bin=$(WL54CF_FW_DIR)/cf8385_helper.bin firmware/cf8385.bin=$(WL54CF_FW_DIR)/cf8385.bin

.PHONY: hwpkg-files hwpkgs hwpkg-wl54cf
