# ---- SPL --------------------------------------------------------------------

# The SPL reads the kernel's load address, entry and size from the kernel
# slot's header (glaze/ota.h), so it isn't built against a particular kernel.
# It applies OTA updates (boot/ota_apply.c) through a read-only build of the
# file server's flash stack.
SPL_CFLAGS := $(CFLAGS_COMMON) -Os -ffunction-sections -fdata-sections $(if $(BOOT_BEEP),-DSPL_BOOT_BEEP)
SPL_FS_DEFS := -DFTL_READONLY -DGF13_NO_TABLES -DLFS_READONLY -DLFS_NO_MALLOC -DLFS_NO_DEBUG -DLFS_NO_WARN \
    -DLFS_NO_ERROR -DLFS_NO_ASSERT -Ithird_party/littlefs -Ithird_party/dhara
SPL_FS_SRCS := servers/fs/ftl.c third_party/puff/puff.c third_party/littlefs/lfs.c third_party/littlefs/lfs_util.c \
    third_party/dhara/dhara/map.c third_party/dhara/dhara/journal.c third_party/dhara/dhara/error.c \
    third_party/dhara/ecc/bch.c third_party/dhara/ecc/gf13.c lib/libc/string.c lib/libc/bitops.c \
    lib/libc/divdi3.c
SPL_FS_OBJS := $(patsubst %.c,$(OUT)/boot/fs/%.o,$(SPL_FS_SRCS))
SPL_OBJS := $(OUT)/boot/spl_start.o $(OUT)/boot/spl.o $(OUT)/boot/ota_apply.o $(OUT)/boot/setjmp.o \
    $(OUT)/boot/nand_boot.o $(OUT)/boot/nand_ecc.o $(SPL_FS_OBJS)

$(SPL_FS_OBJS): $(OUT)/boot/fs/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(SPL_CFLAGS) $(SPL_FS_DEFS) $(if $(filter third_party/%,$<),-w) -c -o $@ $<
$(OUT)/boot/ota_apply.o: SPL_CFLAGS += $(SPL_FS_DEFS) -Ithird_party/puff
$(OUT)/boot/fs/third_party/puff/puff.o: SPL_FS_DEFS += -Iboot/include

# Rebuild the SPL when BOOT_BEEP changes.
$(OUT)/boot/spl.flags: FORCE
	@mkdir -p $(dir $@)
	@echo '$(SPL_CFLAGS)' | cmp -s - $@ || echo '$(SPL_CFLAGS)' > $@

$(OUT)/boot/spl.o: $(OUT)/boot/spl.flags

$(OUT)/boot/%.o: boot/%.S kernel/drivers/nand_boot.h kernel/resume.h
	@mkdir -p $(dir $@)
	$(CC) $(SPL_CFLAGS) -c -o $@ $<

$(OUT)/boot/%.o: boot/%.c kernel/drivers/nand_boot.h kernel/resume.h lib/include/glaze/ota.h boot/ota_apply.h boot/splash.h
	@mkdir -p $(dir $@)
	$(CC) $(SPL_CFLAGS) -c -o $@ $<

$(OUT)/boot/nand_%.o: kernel/drivers/nand_%.c kernel/drivers/nand_boot.h kernel/drivers/nand_ecc.h
	@mkdir -p $(dir $@)
	$(CC) $(SPL_CFLAGS) -c -o $@ $<

$(OUT)/spl.elf: $(SPL_OBJS) boot/spl.lds
	$(LD) -m elf32ltsmip --gc-sections -T boot/spl.lds -o $@ $(SPL_OBJS)
