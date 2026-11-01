# ---- app packages -------------------------------------------------------------
# Apps that are installed rather than built in (glaze/pkg.h). packages/<name>
# holds the sources, Info and icon.gzi; the build links the app like a built-in
# one, never puts it in romfs or on the factory volume, and packs it as
# out/pkg/<name>.gzp, to be installed from a card (tools/mkcfimg.sh).
# Installed apps run on the dynamic linker, so there are none in a DYNAMIC=0
# build.

PKG_DIRS := $(patsubst %/,%,$(wildcard packages/*/))
PKG_NAMES := $(notdir $(PKG_DIRS))
ifeq ($(DYNAMIC),1)
$(foreach d,$(PKG_DIRS),$(eval $(call DYNAMIC_template,$(d))))
PKG_FILES := $(foreach p,$(PKG_NAMES),$(OUT)/pkg/$(p).gzp)
else
PKG_FILES :=
endif

# Files in packages/<name>/res/ go in as res/<file>, next to the icon.
PKG_RES = $(wildcard packages/$(1)/res/*)

.SECONDEXPANSION:
$(OUT)/pkg/%.gzp: $(OUT)/bin/%.elf packages/%/Info packages/%/icon.gzi $$(call PKG_RES,$$*) tools/mkromfs.py
	@mkdir -p $(dir $@)
	$(OBJCOPY) --strip-all $< $(OUT)/pkg/$*.app
	$(PYTHON) tools/mkromfs.py --package $@ Info=packages/$*/Info app=$(OUT)/pkg/$*.app \
	    res/icon.gzi=packages/$*/icon.gzi $(foreach f,$(call PKG_RES,$*),res/$(notdir $(f))=$(f))

packages: $(PKG_FILES)
