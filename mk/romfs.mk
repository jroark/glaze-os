# ---- romfs ------------------------------------------------------------------

ROMFS_BINS := $(foreach p,$(PROGS),$(OUT)/romfs/bin/$(p))

# mkromfs arguments for programs $(1) and assets $(2).
ROMFS_ARGS = etc/services=$(OUT)/services.bin \
	    $(foreach d,$(DRIVER_FILES),drivers/$(notdir $(d))=$(d)) \
	    $(foreach p,$(1),bin/$(p)=$(OUT)/romfs/bin/$(p)) \
	    $(foreach l,$(ROMFS_LIBRARY_FILES),lib/$(notdir $(l))=$(l)) \
	    $(foreach a,$(2),res/$(notdir $(a))=$(a)) \
	    $(foreach w,$(WEB_FILES),web/$(notdir $(w))=$(w)) \
	    $(foreach c,$(SSL_FILES),ssl/$(notdir $(c))=$(c)) \
	    $(foreach s,$(SOUND_FILES),sounds/$(notdir $(s))=$(s)) \
	    $(foreach f,$(WL54CF_FW),firmware/$(notdir $(f))=$(f)) \
	    $(if $(INPUT_SCRIPT),test/input.txt=$(INPUT_SCRIPT))

# NAND_DROP leaves apps out of the NAND romfs to make room, as GB_APPS does
# for GlazeBoot. SpringBoard hides a built-in app whose program is missing.
# A NAND-booted device has no Windows CE partitions left for bin/wince, so
# NAND_DROP=wince is what makes room for WL54CF=1's firmware.
NAND_DROP ?=
NAND_PROGS := $(filter-out $(NAND_DROP),$(PROGS))
NAND_ASSETS := $(filter-out $(foreach a,$(NAND_DROP),assets/gen/icon_$(a).gzi),$(ASSET_FILES))

$(OUT)/romfs.img: $(DRIVER_FILES) $(OUT)/services.bin $(ROMFS_LIBRARY_FILES) $(ROMFS_BINS) $(ASSET_FILES) $(WEB_FILES) $(SSL_FILES) $(SOUND_FILES) $(WL54CF_FW) tools/mkromfs.py
	$(PYTHON) tools/mkromfs.py $@ $(call ROMFS_ARGS,$(NAND_PROGS),$(NAND_ASSETS))
