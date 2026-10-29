# Corresponding source for the SDK's startup objects and shared libraries.
# Run `make -C src` from the extracted SDK. No Linux libc or libgcc is linked.
CROSS ?= mipsel-linux-gnu-
CC := $(CROSS)gcc
LD := $(CROSS)ld
OUT ?= rebuilt
ARCH := -march=mips2 -mabi=32 -msoft-float -G0
BASE := $(ARCH) -std=gnu11 -ffreestanding -fno-builtin -nostdlib -fno-stack-protector \
    -O2 -g -Wall -Wextra -Wno-unused-parameter -ffunction-sections -fdata-sections \
    -Ilib/include -Ilib/libc/include -MMD -MP
PIC := $(BASE) -mabicalls -fPIC
LDFLAGS := -m elf32ltsmip --hash-style=sysv -z now -z relro -z text -z max-page-size=4096 -z separate-code
LIBS := c gfx ui media
LIBFILES := $(foreach n,$(LIBS),$(OUT)/lib$(n).so.1)
.DEFAULT_GOAL := all
.PHONY: all
all: $(LIBFILES) $(OUT)/crt0.o $(OUT)/start.o $(OUT)/ld-glaze.so.1
$(OUT)/%.o: lib/%.c
	@mkdir -p $(dir $@)
	$(CC) $(PIC) -c $< -o $@
$(OUT)/crt0.o: lib/crt0_pic.S
	@mkdir -p $(dir $@)
	$(CC) $(PIC) -c $< -o $@
$(OUT)/start.o: lib/libc/start.c
	@mkdir -p $(dir $@)
	$(CC) $(PIC) -c $< -o $@
define LIBRARY
$(OUT)/lib$(1).so.1: $$(patsubst lib/%.c,$(OUT)/%.o,$$(filter-out lib/libc/start.c,$$(wildcard lib/$(2)/*.c))) lib/$(2)/exports.map $(3)
	$(LD) $(LDFLAGS) -shared -soname lib$(1).so.1 -z defs --gc-sections --version-script=lib/$(2)/exports.map \
	    -o $$@ $$(filter %.o,$$^) -L$(OUT) --as-needed $(4)
endef
$(eval $(call LIBRARY,c,libc,,))
$(eval $(call LIBRARY,gfx,gfx,$(OUT)/libc.so.1,-l:libc.so.1))
$(eval $(call LIBRARY,ui,ui,$(OUT)/libgfx.so.1,-l:libgfx.so.1 -l:libc.so.1))
$(eval $(call LIBRARY,media,media,$(OUT)/libc.so.1,-l:libc.so.1))
$(OUT)/ldso/%.o: lib/ldso/%.c
	@mkdir -p $(dir $@)
	$(CC) $(BASE) -mno-abicalls -fno-pic -Os -c $< -o $@
$(OUT)/ldso/%.o: lib/ldso/%.S
	@mkdir -p $(dir $@)
	$(CC) $(BASE) -mno-abicalls -fno-pic -Os -c $< -o $@
$(OUT)/ldso/%.o: lib/libc/%.c
	@mkdir -p $(dir $@)
	$(CC) $(BASE) -mno-abicalls -fno-pic -Os -c $< -o $@
$(OUT)/ld-glaze.so.1: $(addprefix $(OUT)/ldso/,entry.o ldso.o string.o divdi3.o) lib/ldso/ldso.lds
	$(LD) -m elf32ltsmip -z max-page-size=4096 -T lib/ldso/ldso.lds --gc-sections -o $@ $(filter %.o,$^)
-include $(shell find $(OUT) -name '*.d' 2>/dev/null)
