# Pinned engine; no WAD/game data is downloaded or packed by the build.
DOOM_SRC := $(wildcard third_party/doomgeneric/*.c)
PROG_EXTRA_doom := $(DOOM_SRC)
PROG_CFLAGS_doom := -Ithird_party/doomgeneric -include packages/doom/compat.h -fno-strict-aliasing -fwrapv
# Retain upstream warning policy separately from Glaze glue.
$(patsubst %.c,$(OUT)/dyn/%.o,$(DOOM_SRC)): PIC_CFLAGS += -Wno-unused-function -Wno-unused-variable -Wno-sign-compare -Wno-type-limits -Wno-missing-field-initializers -Wno-implicit-fallthrough -Wno-unused-but-set-parameter -Wno-enum-conversion
$(OUT)/dyn/third_party/doomgeneric/%.o: third_party/doomgeneric/%.c
	@mkdir -p $(dir $@)
	$(CC) $(PIC_CFLAGS) $(PROG_CFLAGS_doom) -c -o $@ $<
