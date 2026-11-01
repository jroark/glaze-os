# ---- user programs ----------------------------------------------------------

PROG_DIRS := $(patsubst %/,%,$(wildcard apps/*/ servers/*/))
ifeq ($(NETDEBUG_TESTS),1)
PROG_DIRS += tests/debug_guest
PROG_CFLAGS_debug_guest := $(WOLFSSL_INC) -DDEBUG_RELOAD_ADDR='"$(or $(DEBUG_RELOAD_ADDR),10.0.0.254)"'
PROG_DEPS_debug_guest := $(OUT)/shared/libwolfssl.so.1
PROG_LDLIBS_debug_guest := -l:libwolfssl.so.1
$(OUT)/dyn/tests/debug_guest/main.o: FORCE
endif
ifeq ($(SERVICE_TESTS),1)
PROG_DIRS += tests/service_guest tests/service_hotplug
endif
ifeq ($(RADIO_TESTS),1)
PROG_DIRS += tests/radio_guest
RADIO_TEST_URL ?= https://somafm.com/groovesalad.pls
PROG_CFLAGS_radio_guest := -DRADIO_TEST_URL='"$(RADIO_TEST_URL)"'
endif
ifneq ($(LD_TESTS),1)
PROG_DIRS := $(filter-out apps/ldtest apps/ldtest_peer apps/ldtest_ro apps/ldtest_oom,$(PROG_DIRS))
endif
ifneq ($(MEM_TESTS),1)
PROG_DIRS := $(filter-out apps/ldmem,$(PROG_DIRS))
endif
# The self-test only goes in images that boot it, to leave romfs room.
ifneq ($(INIT),bin/ktest)
PROG_DIRS := $(filter-out apps/ktest apps/ktest_pong,$(PROG_DIRS))
endif
PROGS := $(notdir $(PROG_DIRS))

$(OUT)/apps/%.o: apps/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c -o $@ $<

$(OUT)/servers/%.o: servers/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c -o $@ $<

# Bootstrap services are independent of the dynamic linker and file server.
STATIC_PROGS := fs net

define STATIC_template
$(OUT)/bin/$(notdir $(1)).elf: $$(patsubst %.c,$(OUT)/%.o,$$(wildcard $(1)/*.c) $$(PROG_EXTRA_$(notdir $(1)))) $(OUT)/lib/crt0.o $(USER_LIBS) $$(PROG_STATIC_DEPS_$(notdir $(1))) lib/user.lds
	@mkdir -p $$(dir $$@)
	$(LD) -m elf32ltsmip -T lib/user.lds --gc-sections -o $$@ $(OUT)/lib/crt0.o \
	    $$(patsubst %.c,$(OUT)/%.o,$$(wildcard $(1)/*.c) $$(PROG_EXTRA_$(notdir $(1)))) \
	    --start-group $(USER_LIBS) $$(PROG_STATIC_DEPS_$(notdir $(1))) --end-group
endef

define DYNAMIC_template
$(OUT)/dyn/$(1)/%.o: $(1)/%.c
	@mkdir -p $$(dir $$@)
	$(CC) $(PIC_CFLAGS) $(PROG_CFLAGS_$(notdir $(1))) -c -o $$@ $$<
$(OUT)/bin/$(notdir $(1)).elf: $$(patsubst %.c,$(OUT)/dyn/%.o,$$(wildcard $(1)/*.c) $$(PROG_EXTRA_$(notdir $(1)))) $(OUT)/pic/crt0.o $(OUT)/pic/start.o $(SHARED_LIBS) \
    $(PROG_DEPS_$(notdir $(1)))
	@mkdir -p $$(dir $$@)
	$(LD) $(DYN_LDFLAGS) $(PROG_LDFLAGS_$(notdir $(1))) -pie -e _start --export-dynamic --gc-sections \
	    --dynamic-linker /lib/ld-glaze.so.1 -o $$@ $(OUT)/pic/crt0.o $(OUT)/pic/start.o \
	    $$(patsubst %.c,$(OUT)/dyn/%.o,$$(wildcard $(1)/*.c) $$(PROG_EXTRA_$(notdir $(1)))) \
	    -L$(OUT)/shared --as-needed $(PROG_LDLIBS_$(notdir $(1))) -l:libui.so.1 -l:libgfx.so.1 -l:libc.so.1
endef

PIC_C_SRCS := $(filter-out lib/libc/start.c,$(LIBC_SRCS))
SHARED_LIBS := $(OUT)/shared/libc.so.1 $(OUT)/shared/libgfx.so.1 $(OUT)/shared/libui.so.1
$(OUT)/pic/%.o: lib/%.c
	@mkdir -p $(dir $@)
	$(CC) $(PIC_CFLAGS) -c -o $@ $<
$(OUT)/pic/crt0.o: lib/crt0_pic.S
	@mkdir -p $(dir $@)
	$(CC) $(PIC_CFLAGS) -c -o $@ $<
$(OUT)/pic/start.o: lib/libc/start.c
	@mkdir -p $(dir $@)
	$(CC) $(PIC_CFLAGS) -c -o $@ $<

$(OUT)/shared/libc.so.1: $(patsubst lib/%.c,$(OUT)/pic/%.o,$(PIC_C_SRCS)) lib/libc/exports.map
	@mkdir -p $(dir $@)
	$(LD) $(DYN_LDFLAGS) -shared -soname libc.so.1 -z defs --gc-sections --version-script=lib/libc/exports.map -o $@ $(filter %.o,$^)
$(OUT)/shared/libgfx.so.1: $(patsubst lib/%.c,$(OUT)/pic/%.o,$(LIBGFX_SRCS)) $(OUT)/shared/libc.so.1 lib/gfx/exports.map
	$(LD) $(DYN_LDFLAGS) -shared -soname libgfx.so.1 -z defs --gc-sections --version-script=lib/gfx/exports.map -o $@ $(filter %.o,$^) -L$(OUT)/shared --as-needed -l:libc.so.1
$(OUT)/shared/libui.so.1: $(patsubst lib/%.c,$(OUT)/pic/%.o,$(LIBUI_SRCS)) $(OUT)/shared/libgfx.so.1 lib/ui/exports.map
	$(LD) $(DYN_LDFLAGS) -shared -soname libui.so.1 -z defs --gc-sections --version-script=lib/ui/exports.map -o $@ $(filter %.o,$^) -L$(OUT)/shared --as-needed -l:libgfx.so.1 -l:libc.so.1

# libmedia is shared by the audio server and Videos.
$(OUT)/shared/libmedia.so.1: $(patsubst lib/%.c,$(OUT)/pic/%.o,$(LIBMEDIA_SRCS)) $(OUT)/shared/libc.so.1 lib/media/exports.map
	$(LD) $(DYN_LDFLAGS) -shared -soname libmedia.so.1 -z defs --gc-sections --version-script=lib/media/exports.map -o $@ $(filter %.o,$^) -L$(OUT)/shared --as-needed -l:libc.so.1

HELIX_PIC_OBJS := $(patsubst %.c,$(OUT)/pic/%.o,$(HELIX_SRCS))
WOLFSSL_PIC_OBJS := $(patsubst %.c,$(OUT)/pic/%.o,$(WOLFSSL_SRCS))
$(HELIX_PIC_OBJS): TP_INC := $(HELIX_INC)
$(WOLFSSL_PIC_OBJS): TP_INC := $(WOLFSSL_INC) -Os
$(OUT)/pic/third_party/%.o: third_party/%.c
	@mkdir -p $(dir $@)
	$(CC) $(filter-out -mno-abicalls -fno-pic,$(ARCHFLAGS)) -mabicalls -fPIC -std=gnu11 -ffreestanding \
	    -fno-builtin -nostdlib -fno-stack-protector -O2 -ffunction-sections -fdata-sections -MMD -g \
	    -Ilib/include -Ilib/libc/include $(TP_INC) -c -o $@ $<
$(OUT)/pic/libhelix.a: $(HELIX_PIC_OBJS)
	rm -f $@
	$(AR) rcs $@ $^
# The browser's alone, like libhelix.a.
$(OUT)/pic/libwolfssl.a: $(WOLFSSL_PIC_OBJS)
	rm -f $@
	$(AR) rcs $@ $^

# TLS code is shared by Browser, App Store, radio and the development service.
# Its only application-provided imports are glaze_tls_seed/time; those are
# exported by each executable and resolved by ld-glaze at process startup.
$(OUT)/shared/libwolfssl.so.1: $(WOLFSSL_PIC_OBJS) $(OUT)/shared/libc.so.1 lib/wolfssl.exports.map
	@mkdir -p $(dir $@)
	$(LD) $(DYN_LDFLAGS) -shared -soname libwolfssl.so.1 --gc-sections --version-script=lib/wolfssl.exports.map -o $@ $(filter %.o,$^) -L$(OUT)/shared --as-needed -l:libc.so.1

JERRY_PIC_OBJS := $(patsubst %.c,$(OUT)/pic/%.o,$(JERRY_SRCS))
$(JERRY_PIC_OBJS): TP_INC := $(JERRY_INC)
$(OUT)/pic/libjerry.a: $(JERRY_PIC_OBJS)
	@mkdir -p $(dir $@)
	rm -f $@
	$(AR) rcs $@ $^

$(OUT)/ldso/%.o: lib/ldso/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Os -c -o $@ $<
$(OUT)/ldso/%.o: lib/ldso/%.S
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c -o $@ $<
$(OUT)/ldso/string.o: lib/libc/string.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Os -c -o $@ $<
$(OUT)/ldso/divdi3.o: lib/libc/divdi3.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -Os -c -o $@ $<
$(OUT)/shared/ld-glaze.so.1: $(OUT)/ldso/entry.o $(OUT)/ldso/ldso.o $(OUT)/ldso/string.o $(OUT)/ldso/divdi3.o lib/ldso/ldso.lds
	@mkdir -p $(dir $@)
	$(LD) -m elf32ltsmip -z max-page-size=4096 -T lib/ldso/ldso.lds --gc-sections -o $@ $(filter %.o,$^)

ifeq ($(DYNAMIC),1)
$(foreach d,$(PROG_DIRS),$(if $(filter $(notdir $(d)),$(STATIC_PROGS)),$(eval $(call STATIC_template,$(d))),$(eval $(call DYNAMIC_template,$(d)))))
ROMFS_LIBS := $(SHARED_LIBS) $(OUT)/shared/libmedia.so.1 $(OUT)/shared/libwolfssl.so.1 $(OUT)/shared/ld-glaze.so.1
else
$(foreach d,$(PROG_DIRS),$(eval $(call STATIC_template,$(d))))
ROMFS_LIBS :=
endif

include tests/dynamic/Makefile.inc

# Standalone native app SDK. This does not build or package a NAND image.
SDK_DIR ?= $(OUT)/sdk/glaze-sdk
SDK_ARCHIVE ?= $(OUT)/sdk/glaze-sdk.tar.gz
.PHONY: sdk sdk-archive sdk-test
ifeq ($(DYNAMIC),1)
sdk: $(SHARED_LIBS) $(OUT)/shared/libmedia.so.1 $(OUT)/shared/libwolfssl.so.1 $(OUT)/shared/ld-glaze.so.1 $(OUT)/pic/crt0.o $(OUT)/pic/start.o
	$(PYTHON) tools/mksdk.py --build-dir $(OUT) --output $(SDK_DIR) --revision '$(BUILD_ID)' --version '$(GLAZE_VERSION)' --cross '$(CROSS)'
sdk-archive: sdk
	$(PYTHON) tools/mksdk.py --output $(SDK_DIR) --archive $(SDK_ARCHIVE) --archive-only
sdk-test: sdk
	$(PYTHON) tests/sdk.py --sdk $(SDK_DIR) --cross '$(CROSS)'
else
sdk sdk-archive sdk-test:
	$(error The app SDK requires DYNAMIC=1; use a separate OUT directory for static builds)
endif

# Keep debugging symbols in link artifacts, not in the shipped library archive.
ROMFS_LIBRARY_FILES := $(foreach l,$(ROMFS_LIBS),$(OUT)/romfs/lib/$(notdir $(l)))
define PACKAGE_LIBRARY
$(OUT)/romfs/lib/$(notdir $(1)): $(1)
	@mkdir -p $$(dir $$@)
	$(OBJCOPY) --strip-all $$< $$@
endef
$(foreach l,$(ROMFS_LIBS),$(eval $(call PACKAGE_LIBRARY,$(l))))

PROG_ELFS := $(foreach p,$(PROGS),$(OUT)/bin/$(p).elf)

$(OUT)/romfs/bin/%: $(OUT)/bin/%.elf
	@mkdir -p $(dir $@)
	$(OBJCOPY) --strip-all $< $@
