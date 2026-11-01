# ---- app packages -------------------------------------------------------------
# Apps that are installed rather than built in (glaze/pkg.h). packages/<name>
# holds the sources, Info and icon.gzi; the build links the app like a built-in
# one, never puts it in romfs or on the factory volume, and packs it as
# out/pkg/<name>.gzp, to be installed from a card (tools/mkcfimg.sh).
# Installed apps run on the dynamic linker, so there are none in a DYNAMIC=0
# build.

include packages/doom/build.mk

PKG_DIRS := $(wildcard packages/*)
PKG_NAMES := $(notdir $(PKG_DIRS))
ifeq ($(DYNAMIC),1)
$(foreach d,$(PKG_DIRS),$(eval $(call DYNAMIC_template,$(d))))
PKG_FILES := $(foreach p,$(PKG_NAMES),$(OUT)/pkg/$(p).gzp)
else
PKG_FILES :=
endif

# Files in packages/<name>/res/ go in as res/<file>, next to the icon.
# ROM binaries are generated into ignored build output, never source directories.
PKG_GENERATED_mastersystem := $(OUT)/pkg-res/mastersystem/demo.sms
PKG_GENERATED_gamegear := $(OUT)/pkg-res/gamegear/demo.gg
PKG_GENERATED_turbografx16 := $(OUT)/pkg-res/turbografx16/demo.pce
PKG_GENERATED_genesis := $(OUT)/pkg-res/genesis/demo.gen
PKG_GENERATED_nes := $(OUT)/pkg-res/nes/demo.nes
PKG_GENERATED_gameboy := $(OUT)/pkg-res/gameboy/demo.gb $(OUT)/pkg-res/gameboy/demo.gbc
PKG_RES = $(wildcard packages/$(1)/res/*) $(PKG_GENERATED_$(1))

$(OUT)/pkg-res/mastersystem/demo.sms: tools/mksegademo.py
	$(PYTHON) $< sms --output $@
$(OUT)/pkg-res/gamegear/demo.gg: tools/mksegademo.py
	$(PYTHON) $< gamegear --output $@
$(OUT)/pkg-res/turbografx16/demo.pce: tools/mkpcedemo.py
	$(PYTHON) $< --output $@
$(OUT)/pkg-res/genesis/demo.gen: tools/mksegademo.py
	$(PYTHON) $< genesis --output $@

$(OUT)/pkg-res/nes/demo.nes: tools/mknesdemo.py
	$(PYTHON) $< --output $@
$(OUT)/pkg-res/gameboy/demo.gb: tools/mkgbdemo.py
	$(PYTHON) $< --output-dir $(dir $@) --mode dmg
$(OUT)/pkg-res/gameboy/demo.gbc: tools/mkgbdemo.py
	$(PYTHON) $< --output-dir $(dir $@) --mode cgb

.SECONDEXPANSION:
$(OUT)/pkg/%.gzp: $(OUT)/bin/%.elf packages/%/Info packages/%/icon.gzi $$(call PKG_RES,$$*) tools/mkromfs.py
	@mkdir -p $(dir $@)
	$(OBJCOPY) --strip-all $< $(OUT)/pkg/$*.app
	$(PYTHON) tools/mkromfs.py --package $@ Info=packages/$*/Info app=$(OUT)/pkg/$*.app \
	    res/icon.gzi=packages/$*/icon.gzi $(foreach f,$(call PKG_RES,$*),res/$(notdir $(f))=$(f))

packages: $(PKG_FILES)
