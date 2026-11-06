# ---- kernel -----------------------------------------------------------------

# wolfCrypt's Ed25519 verification and SHA-256 for OTA updates (kernel/ota_stage.c),
# configured by kernel/crypto/user_settings.h.
KCRYPTO_INC := -Ithird_party/wolfssl -Ikernel/crypto -DWOLFSSL_USER_SETTINGS
KCRYPTO_SRCS := $(addprefix third_party/wolfssl/wolfcrypt/src/,ed25519.c ge_low_mem.c fe_low_mem.c sha512.c sha256.c)
KCRYPTO_OBJS := $(patsubst third_party/%.c,$(OUT)/kernel/crypto/%.o,$(KCRYPTO_SRCS))

KERNEL_SRCS := $(filter-out $(foreach d,$(DRIVER_NAMES) $(PCCARD_DRIVERS),kernel/drivers/$(d).c),$(wildcard kernel/*.c kernel/*.S kernel/drivers/*.c))
KERNEL_OBJS := $(patsubst %,$(OUT)/%.o,$(basename $(KERNEL_SRCS))) $(OUT)/kernel/string.o $(OUT)/kernel/divdi3.o \
    $(OUT)/kernel/bitops.o $(KCRYPTO_OBJS)
KERNEL_CFLAGS := $(CFLAGS_COMMON) -O2 -Ikernel -MMD \
    -DGLAZE_BUILD='"$(BUILD_ID)"' -DGLAZE_VERSION='"$(GLAZE_VERSION)"' -DGLAZE_INIT='"$(INIT)"' \
    $(if $(INPUT_SCRIPT),-DGLAZE_INPUT_SCRIPT) \
    $(if $(COLD_SOCKET_TEST),-DGLAZE_COLD_SOCKET_TEST) \
    $(if $(NAND_BOOT_PROBE),-DGLAZE_NAND_BOOT_PROBE) \
    $(if $(HWDUMP),-DGLAZE_HWDUMP) \
    $(if $(VEO_PROBE),-DGLAZE_VEO_PROBE) \
    $(if $(BOOT_BEEP),-DGLAZE_BOOT_BEEP) \
    $(if $(STOWAWAY_TRACE),-DGLAZE_STOWAWAY_TRACE) \
    $(if $(filter 0,$(IDLE_STANDBY)),-DGLAZE_NO_STANDBY) \
    $(if $(HIBERNATE_VERIFY),-DGLAZE_HIBERNATE_VERIFY)

$(OUT)/kernel/%.o: kernel/%.c
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -c -o $@ $<

$(OUT)/kernel/%.o: kernel/%.S
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -c -o $@ $<

# Shared with libc.
$(OUT)/kernel/%.o: lib/libc/%.c
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -c -o $@ $<

$(KCRYPTO_OBJS): $(OUT)/kernel/crypto/%.o: third_party/%.c kernel/crypto/user_settings.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_COMMON) -Os -ffunction-sections -fdata-sections -w $(KCRYPTO_INC) -c -o $@ $<

# The OTA release key (docs/os/OTA.md), as a C array; config/ota_pubkey.bin is
# the 32-byte Ed25519 public key.
# OTA_PUBKEY lets a test image (tests/run_ota.py) trust a throwaway key.
OTA_PUBKEY ?= config/ota_pubkey.bin
$(OUT)/gen/ota_key.h: $(OTA_PUBKEY) tools/mkota.py FORCE
	@mkdir -p $(dir $@)
	$(PYTHON) tools/mkota.py key-header $(OTA_PUBKEY) $@.tmp
	@cmp -s $@.tmp $@ && rm $@.tmp || mv $@.tmp $@
$(OUT)/kernel/ota.o: $(OUT)/gen/ota_key.h
$(OUT)/kernel/ota.o: KERNEL_CFLAGS += -I$(OUT)/gen
$(OUT)/kernel/ota_stage.o: KERNEL_CFLAGS += $(KCRYPTO_INC)

# These bake in INIT/BUILD_ID/INPUT_SCRIPT/VERSION/STOWAWAY_TRACE; rebuild them every time.
$(OUT)/kernel/main.o $(OUT)/kernel/drivers/veo.o $(OUT)/kernel/ota.o $(OUT)/kernel/syscall.o $(OUT)/kernel/drivers/input.o $(OUT)/kernel/drivers/power.o $(OUT)/kernel/drivers/stowaway_hw.o $(OUT)/romfs.img: FORCE

$(OUT)/kernel/reboot.checked: $(OUT)/kernel/reboot_entry.o
	@if $(CROSS)objdump -r -j .text.reboot $< | grep -q 'R_MIPS_'; then \
	    echo "reboot_entry.o: the copied reset code must be position independent" >&2; exit 1; fi
	@touch $@

$(OUT)/kernel.elf: $(KERNEL_OBJS) kernel/kernel.lds $(OUT)/kernel/reboot.checked
	$(LD) -m elf32ltsmip -T kernel/kernel.lds -o $@ $(KERNEL_OBJS)
