# ---- GlazeBoot: boot from RAM out of Windows CE (docs/dev/GLAZEBOOT.md) -----------

# The loader (wince/glazeboot/GlazeBoot.c) is a Windows CE program, built
# on the host with the be300-dev toolchain (clang and the Casio SDK):
# `make glazeboot-exe`. `make glazeboot` builds what it needs from GCC into
# out/glazeboot/gen: the trampoline as bytes (tramp_blob.h) and the layouts
# it shares with the kernel (glazeboot_abi.h). Under ./build.sh the two run
# separately, the first in the container and the second natively.
GB_DIR := $(OUT)/glazeboot
GB_GEN := $(GB_DIR)/gen
$(OUT)/kernel/usb_reload.o: $(GB_GEN)/tramp_blob.h
$(OUT)/kernel/usb_reload.o: KERNEL_CFLAGS += -I$(GB_GEN)
BE300_DEV ?= ../be300-dev
GB_CFLAGS := $(ARCHFLAGS) -std=gnu11 -ffreestanding -fno-builtin -nostdlib \
    -fno-stack-protector -fno-tree-loop-distribute-patterns -Os -Wall -Wextra -Ilib/include -MMD

$(GB_DIR)/tramp.o: wince/glazeboot/tramp.c
	@mkdir -p $(dir $@)
	$(CC) $(GB_CFLAGS) -c -o $@ $<

# The trampoline runs from a copy at another address, so it may only use
# PC-relative branches: no absolute relocations in its sections.
$(GB_DIR)/tramp.checked: $(GB_DIR)/tramp.o
	@if $(CROSS)objdump -r -j .tramp -j .tramp.entry $< | grep -E 'R_MIPS_(26|HI16|LO16|32|GOT)'; then \
	    echo "tramp.o: the trampoline must be position independent" >&2; exit 1; fi
	@touch $@

$(GB_DIR)/tramp.elf: $(GB_DIR)/tramp.o $(GB_DIR)/tramp.checked wince/glazeboot/tramp.lds
	$(LD) -m elf32ltsmip -T wince/glazeboot/tramp.lds -o $@ $<

$(GB_GEN)/tramp_blob.h: $(GB_DIR)/tramp.elf tools/mktrampblob.py
	@mkdir -p $(dir $@)
	$(OBJCOPY) -O binary -j .text $< $(GB_DIR)/tramp.bin
	$(CROSS)nm $< > $(GB_DIR)/tramp.sym
	$(PYTHON) tools/mktrampblob.py $(GB_DIR)/tramp.bin $(GB_DIR)/tramp.sym $@

$(GB_DIR)/mkabi: wince/glazeboot/mkabi.c wince/glazeboot/plan.h lib/include/glaze/bootinfo.h lib/include/glaze/abi.h
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=gnu11 -Wall -Wextra -Ilib/include -o $@ $<

$(GB_GEN)/glazeboot_abi.h: $(GB_DIR)/mkabi
	@mkdir -p $(dir $@)
	$< > $@

$(GB_DIR)/kernel.bin: $(OUT)/kernel.elf tools/mkkernelbin.py
	@mkdir -p $(dir $@)
	$(PYTHON) tools/mkkernelbin.py $< $@

# A RAM boot holds all of romfs in RAM, so GlazeBoot's carries only the apps
# in GB_APPS (SpringBoard shows the built-ins it finds), with their icons;
# servers and test programs always go in. GB_APPS=all keeps every app.
GB_APPS ?= appstore browser files settings wince
GB_TEST_PROGS := ktest ktest_pong ldtest ldtest_peer ldtest_ro ldtest_oom ldmem
ifeq ($(GB_APPS),all)
GB_DROP :=
else
GB_DROP := $(filter-out $(GB_APPS) $(GB_TEST_PROGS),$(notdir $(patsubst %/,%,$(wildcard apps/*/))))
endif
GB_PROGS := $(filter-out $(GB_DROP),$(PROGS))
GB_ASSETS := $(filter-out $(foreach a,$(GB_DROP),assets/gen/icon_$(a).gzi) \
    $(if $(filter clock,$(GB_DROP)),assets/gen/tab_%.gzi),$(ASSET_FILES))

$(GB_DIR)/romfs.img: $(DRIVER_FILES) $(OUT)/services.bin $(ROMFS_LIBRARY_FILES) $(ROMFS_BINS) $(ASSET_FILES) $(WEB_FILES) $(SSL_FILES) $(SOUND_FILES) $(WL54CF_FW) tools/mkromfs.py FORCE
	@mkdir -p $(dir $@)
	$(PYTHON) tools/mkromfs.py $(ROMFS_FLAGS) $@ $(call ROMFS_ARGS,$(GB_PROGS),$(GB_ASSETS))

$(GB_DIR)/glaze.cfg: wince/glazeboot/glaze.cfg
	@mkdir -p $(dir $@)
	cp $< $@

GB_FILES := $(GB_DIR)/kernel.bin $(GB_DIR)/romfs.img $(GB_DIR)/glaze.cfg \
    $(GB_GEN)/tramp_blob.h $(GB_GEN)/glazeboot_abi.h

glazeboot: $(GB_FILES)

# The loader, natively (no GCC needed): the generated headers must exist.
BE300_RT := $(BE300_DEV)/build/rt/libbe300rt.a

$(GB_DIR)/GlazeBoot.o: wince/glazeboot/GlazeBoot.c $(BE300_DEV)/tools/be300-cc $(GB_GEN)/tramp_blob.h $(GB_GEN)/glazeboot_abi.h
	@[ -x $(BE300_DEV)/tools/be300-cc ] && [ -f $(BE300_RT) ] || \
	    { echo "no be300-dev at $(BE300_DEV) (set BE300_DEV, run its setup.sh)" >&2; exit 1; }
	$(BE300_DEV)/tools/be300-cc -Os -Wall -Wextra -Wno-unused-parameter -I$(GB_GEN) -c -o $@ $<

$(GB_DIR)/GlazeBoot.exe: $(GB_DIR)/GlazeBoot.o
	$(BE300_DEV)/tools/be300-ld -o $@ --map $(GB_DIR)/GlazeBoot.map $< $(BE300_RT) \
	    -L $(BE300_DEV)/sdk/Lib -l coredll

glazeboot-exe: $(GB_DIR)/GlazeBoot.exe
.PHONY: glazeboot glazeboot-exe

$(GB_DIR)/UsbProbe.exe: wince/usb_probe/main.c $(BE300_RT)
	@mkdir -p $(GB_DIR)
	$(BE300_DEV)/tools/be300-cc -Os -Wall -Wextra -c -o $(GB_DIR)/UsbProbe.o $<
	$(BE300_DEV)/tools/be300-ld -o $@ $(GB_DIR)/UsbProbe.o $(BE300_RT) -L $(BE300_DEV)/sdk/Lib -l coredll
usb-probe-exe: $(GB_DIR)/UsbProbe.exe

# Veo camera check under stock CE (wince/veo_test, docs/hardware/VEO.md).
$(GB_DIR)/VeoTest.exe: wince/veo_test/main.c $(BE300_RT)
	@mkdir -p $(GB_DIR)
	$(BE300_DEV)/tools/be300-cc -Os -Wall -Wextra -Wno-unused-parameter -c -o $(GB_DIR)/VeoTest.o $<
	$(BE300_DEV)/tools/be300-ld -o $@ $(GB_DIR)/VeoTest.o $(BE300_RT) -L $(BE300_DEV)/sdk/Lib -l coredll
veo-test-exe: $(GB_DIR)/VeoTest.exe
.PHONY: veo-test-exe
.PHONY: usb-probe-exe

# RAM-boot test image (tests/run_ramboot.sh): an SPL that hands over the way
# GlazeBoot does, through its trampoline, and optionally a FAT volume
# (RAMBOOT_FS) in the NAND's file-system partition for /NAND.
RB_DIR := $(OUT)/ramboot
RAMBOOT_FS ?=

$(RB_DIR)/%.o: boot/%.S kernel/drivers/nand_boot.h kernel/resume.h
	@mkdir -p $(dir $@)
	$(CC) $(SPL_CFLAGS) -DSPL_RAMBOOT -c -o $@ $<

$(RB_DIR)/%.o: boot/%.c kernel/drivers/nand_boot.h kernel/resume.h lib/include/glaze/ota.h boot/ota_apply.h boot/splash.h
	@mkdir -p $(dir $@)
	$(CC) $(SPL_CFLAGS) -DSPL_RAMBOOT -c -o $@ $<

RB_OBJS := $(RB_DIR)/spl_start.o $(RB_DIR)/spl.o $(GB_DIR)/tramp.o $(filter-out $(OUT)/boot/spl_start.o $(OUT)/boot/spl.o,$(SPL_OBJS))
$(RB_DIR)/spl.elf: $(RB_OBJS) $(GB_DIR)/tramp.checked boot/spl.lds
	$(LD) -m elf32ltsmip --gc-sections -T boot/spl.lds -o $@ $(RB_OBJS)

$(OUT)/glaze-ramboot.bin: $(RB_DIR)/spl.elf $(OUT)/kernel.elf $(OUT)/romfs.img $(RAMBOOT_FS) tools/mk_nand.py
	$(PYTHON) tools/mk_nand.py --kernel $(OUT)/kernel.elf --spl $(RB_DIR)/spl.elf $(MK_NAND_VERSION) \
	    --romfs $(OUT)/romfs.img $(if $(RAMBOOT_FS),--fs-image $(RAMBOOT_FS)) --out $@

ramboot: $(OUT)/glaze-ramboot.bin
.PHONY: ramboot
