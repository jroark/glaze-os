# ---- factory file system ------------------------------------------------------
# User files the device ships with (userdata/), formatted into the file
# server's region by the same flash stack it runs. Romfs holds only the OS.
# FS_EXTRA adds build products as /path=file pairs, creating their folders:
# FS_EXTRA="/Maps/city.gzmap=out/maps/city.gzmap" puts a map pack in a
# NAND image without committing it (docs/hardware/GPS.md).

FS_BLOCKS := $(shell sed -n 's/^\#define GZ_FLASH_BLOCKS *\([0-9]*\).*/\1/p' lib/include/glaze/abi.h)
USERDATA_FILES := $(shell find userdata -type f ! -name '.*')
MKFSIMG_SRCS := host/mkfsimg.c servers/fs/ftl.c $(FSSTACK_SRCS)

$(OUT)/buildtools/mkfsimg: $(MKFSIMG_SRCS) servers/fs/ftl.h servers/fs/folders.h
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=gnu11 -O2 -Wall -Iservers/fs $(FS_INC) -o $@ $(MKFSIMG_SRCS)

$(OUT)/wifi-entropy:
	@mkdir -p $(dir $@)
	$(PYTHON) -c 'import os; open("$@", "wb").write(os.urandom(32))'

FS_EXTRA_FILES := $(foreach e,$(FS_EXTRA),$(lastword $(subst =, ,$(e))))
$(OUT)/fs.img: $(OUT)/wifi-entropy $(OUT)/buildtools/mkfsimg $(USERDATA_FILES) $(FS_EXTRA_FILES) lib/include/glaze/abi.h
	$(OUT)/buildtools/mkfsimg userdata $(FS_BLOCKS) $@ /Library/Preferences/net.entropy=$(OUT)/wifi-entropy $(FS_EXTRA)
