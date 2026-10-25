# Glaze OS for the Casio BE-300.
#
# Run inside the mips-cross-dev container (see ./build.sh), which provides
# the mipsel-linux-gnu toolchain and python3.

CROSS   ?= mipsel-linux-gnu-
CC      := $(CROSS)gcc
LD      := $(CROSS)ld
AR      := $(CROSS)ar
OBJCOPY := $(CROSS)objcopy
PYTHON  ?= python3
# Native compiler for build tools (host/mkfsimg).
HOSTCC  ?= cc

OUT     ?= out
DYNAMIC ?= 1

# First user process started by the kernel.
INIT    ?= bin/springboard
BUILD_ID ?= $(shell git describe --always --dirty 2>/dev/null || echo dev)
# Test builds: replay this input script (see kernel/drivers/input.c).
INPUT_SCRIPT ?=

ARCHFLAGS := -march=mips2 -mabi=32 -mno-abicalls -fno-pic -G 0 -msoft-float
CFLAGS_COMMON := $(ARCHFLAGS) -std=gnu11 -ffreestanding -fno-builtin -nostdlib \
    -fno-stack-protector -Wall -Wextra -Wno-unused-parameter -g -Ilib/include -Ilib/libc/include

.DEFAULT_GOAL := all

# ---- userspace libraries ----------------------------------------------------

USER_CFLAGS := $(CFLAGS_COMMON) -O2 -ffunction-sections -fdata-sections -MMD
PIC_CFLAGS := $(filter-out -mno-abicalls -fno-pic,$(CFLAGS_COMMON)) -mabicalls -fPIC -O2 -ffunction-sections -fdata-sections -MMD
DYN_LDFLAGS := -m elf32ltsmip --hash-style=sysv -z now -z relro -z text -z max-page-size=4096 -z separate-code

LIBC_SRCS := $(wildcard lib/libc/*.c)
LIBGFX_SRCS := $(wildcard lib/gfx/*.c)
LIBUI_SRCS := $(wildcard lib/ui/*.c)

$(OUT)/lib/%.o: lib/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c -o $@ $<

$(OUT)/lib/crt0.o: lib/crt0.S
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c -o $@ $<

define LIB_template
$(OUT)/lib/lib$(1).a: $$(patsubst lib/%.c,$(OUT)/lib/%.o,$(2))
	@mkdir -p $$(dir $$@)
	rm -f $$@
	$(AR) rcs $$@ $$^
endef
$(eval $(call LIB_template,c,$(LIBC_SRCS)))
$(eval $(call LIB_template,gfx,$(LIBGFX_SRCS)))
$(eval $(call LIB_template,ui,$(LIBUI_SRCS)))

# littlefs and Dhara for the file server's flash, FatFs for CompactFlash
# cards (third_party/README.md).
FS_INC := -Ithird_party/littlefs -Ithird_party/dhara -Ithird_party/fatfs -DLFS_NO_DEBUG \
    -DLFS_NO_WARN -DLFS_NO_ERROR -DLFS_NO_ASSERT
FSSTACK_SRCS := third_party/littlefs/lfs.c third_party/littlefs/lfs_util.c \
    third_party/dhara/dhara/map.c third_party/dhara/dhara/journal.c \
    third_party/dhara/dhara/error.c third_party/dhara/ecc/bch.c third_party/dhara/ecc/gf13.c
FATFS_SRCS := third_party/fatfs/ff.c third_party/fatfs/ffunicode.c
FSSTACK_OBJS := $(patsubst %.c,$(OUT)/%.o,$(FSSTACK_SRCS) $(FATFS_SRCS))

# lwIP for the network server; its port headers live in servers/net.
NET_INC := -Ithird_party/lwip/src/include -Iservers/net
NETSTACK_SRCS := $(wildcard third_party/lwip/src/core/*.c third_party/lwip/src/core/ipv4/*.c \
    third_party/lwip/src/api/*.c) \
    third_party/lwip/src/netif/ethernet.c
NETSTACK_OBJS := $(patsubst %.c,$(OUT)/%.o,$(NETSTACK_SRCS))

$(OUT)/third_party/%.o: third_party/%.c
	@mkdir -p $(dir $@)
	$(CC) $(ARCHFLAGS) -std=gnu11 -ffreestanding -fno-builtin -nostdlib -fno-stack-protector \
	    -O2 -ffunction-sections -fdata-sections -MMD -g -Ilib/include -Ilib/libc/include \
	    $(TP_INC) -c -o $@ $<

# The Helix fixed-point MP3 decoder. Static builds put it in libmedia.a with
# lib/media; dynamic ones keep it a private archive of bin/audio, its only user.
HELIX_INC := -Ithird_party/helix-mp3/pub
HELIX_SRCS := $(wildcard third_party/helix-mp3/*.c third_party/helix-mp3/real/*.c)
HELIX_OBJS := $(patsubst %.c,$(OUT)/%.o,$(HELIX_SRCS))
LIBMEDIA_SRCS := $(wildcard lib/media/*.c)

# wolfSSL for the browser's https, configured by apps/browser/tls/user_settings.h.
# ssl.c pulls in the other src/*.c files it needs.
WOLFSSL_INC := -Ithird_party/wolfssl -Iapps/browser/tls -DWOLFSSL_USER_SETTINGS
WOLFSSL_SRCS := $(addprefix third_party/wolfssl/,src/ssl.c src/internal.c src/tls.c src/tls13.c \
    src/keys.c src/wolfio.c $(addprefix wolfcrypt/src/,aes.c sha.c sha256.c sha512.c hmac.c hash.c \
    kdf.c random.c rsa.c ecc.c curve25519.c fe_low_mem.c chacha.c poly1305.c chacha20_poly1305.c \
    sp_int.c wolfmath.c asn.c coding.c memory.c wc_port.c wc_encrypt.c error.c logging.c))
WOLFSSL_OBJS := $(patsubst %.c,$(OUT)/%.o,$(WOLFSSL_SRCS))

# Browser-private JavaScript engine, math and software floating-point builtins.
JERRY_SRCS := third_party/jerryscript/jerryscript.c third_party/jerryscript/jerryscript-math.c $(wildcard third_party/compiler-rt/*.c)
JERRY_INC := -I$(OUT)/include -Ithird_party/jerryscript -Iapps/browser/js_platform -include apps/browser/js_config.h -Os -fvisibility=hidden
JERRY_OBJS := $(patsubst %.c,$(OUT)/%.o,$(JERRY_SRCS))
$(JERRY_OBJS): TP_INC := $(JERRY_INC)
$(OUT)/lib/libjerry.a: $(JERRY_OBJS)
	@mkdir -p $(dir $@)
	rm -f $@
	$(AR) rcs $@ $^

$(FSSTACK_OBJS): TP_INC := $(FS_INC)
$(NETSTACK_OBJS): TP_INC := $(NET_INC)
$(HELIX_OBJS): TP_INC := $(HELIX_INC)
$(WOLFSSL_OBJS): TP_INC := $(WOLFSSL_INC) -Os

$(OUT)/lib/libfsstack.a: $(FSSTACK_OBJS)
	@mkdir -p $(dir $@)
	rm -f $@
	$(AR) rcs $@ $^

$(OUT)/lib/libnetstack.a: $(NETSTACK_OBJS)
	@mkdir -p $(dir $@)
	rm -f $@
	$(AR) rcs $@ $^

$(OUT)/lib/libwolfssl.a: $(WOLFSSL_OBJS)
	@mkdir -p $(dir $@)
	rm -f $@
	$(AR) rcs $@ $^

$(OUT)/lib/libmedia.a: $(patsubst lib/%.c,$(OUT)/lib/%.o,$(LIBMEDIA_SRCS)) $(HELIX_OBJS)
	@mkdir -p $(dir $@)
	rm -f $@
	$(AR) rcs $@ $^

$(OUT)/servers/fs/%.o: USER_CFLAGS += $(FS_INC)
$(OUT)/servers/net/%.o: USER_CFLAGS += $(NET_INC)
$(OUT)/servers/audio/%.o: USER_CFLAGS += $(HELIX_INC) -Iapps/browser $(WOLFSSL_INC)
$(OUT)/apps/browser/%.o: USER_CFLAGS += -Ithird_party/stb $(WOLFSSL_INC) $(JERRY_INC)
$(OUT)/apps/video/%.o: USER_CFLAGS += -Ithird_party/stb

# Per-program additions for dynamic links: compile flags, link inputs (built
# first) and libraries.
# The native store shares the Browser transport, not its renderer.
PROG_EXTRA_appstore := apps/browser/http.c apps/browser/conn.c apps/browser/tls.c apps/browser/url.c
APPSTORE_URL ?= https://appstore.glaze-os.com
PROG_CFLAGS_appstore := -DAPPSTORE_URL='"$(APPSTORE_URL)"' -Iapps/browser -Ithird_party/stb $(WOLFSSL_INC)
PROG_DEPS_appstore := $(OUT)/shared/libwolfssl.so.1
PROG_LDLIBS_appstore := -l:libwolfssl.so.1
$(OUT)/apps/appstore/%.o: USER_CFLAGS += -DAPPSTORE_URL='"$(APPSTORE_URL)"' -Iapps/browser -Ithird_party/stb $(WOLFSSL_INC)
# Pick up build-time endpoint changes without requiring a clean build.
$(OUT)/apps/appstore/main.o $(OUT)/dyn/apps/appstore/main.o: FORCE


PROG_CFLAGS_browser := -Ithird_party/stb $(WOLFSSL_INC) $(JERRY_INC)
PROG_CFLAGS_video := -Ithird_party/stb
PROG_EXTRA_audio := apps/browser/conn.c apps/browser/tls.c apps/browser/url.c
PROG_CFLAGS_audio := $(HELIX_INC) -Iapps/browser $(WOLFSSL_INC)
# CE entry points have a uniform twelve-word C signature, but original callers
# reserve stack space only for the real API's arguments. A sibling call to
# another adapter may otherwise spill unused words over the caller's saved ra.
PROG_CFLAGS_wince := -fno-optimize-sibling-calls
$(OUT)/apps/wince/%.o: USER_CFLAGS += -fno-optimize-sibling-calls
$(patsubst %.c,$(OUT)/%.o,$(wildcard apps/wince/*.c)) \
$(patsubst %.c,$(OUT)/dyn/%.o,$(wildcard apps/wince/*.c)): Makefile
PROG_DEPS_video := $(OUT)/shared/libmedia.so.1
PROG_LDLIBS_video := -l:libmedia.so.1
PROG_DEPS_audio := $(OUT)/pic/libhelix.a $(OUT)/shared/libwolfssl.so.1 $(OUT)/shared/libmedia.so.1
PROG_LDLIBS_audio := $(OUT)/pic/libhelix.a -l:libwolfssl.so.1 -l:libmedia.so.1
PROG_DEPS_browser := $(OUT)/shared/libwolfssl.so.1 $(OUT)/pic/libjerry.a
PROG_LDLIBS_browser := -l:libwolfssl.so.1 $(OUT)/pic/libjerry.a
PROG_STATIC_DEPS_browser := $(OUT)/lib/libjerry.a
PROG_LDFLAGS_browser := --exclude-libs ALL
PROG_CFLAGS_gmail := $(WOLFSSL_INC)
PROG_DEPS_gmail := $(OUT)/pic/libwolfssl.a
PROG_LDLIBS_gmail := $(OUT)/pic/libwolfssl.a

# InfoNES is portable C despite upstream's historical .cpp filenames.
PROG_CFLAGS_nes := -fno-strict-aliasing
PROG_CFLAGS_gameboy := -fno-strict-aliasing -DIS_LITTLE_ENDIAN
PROG_CFLAGS_mastersystem := -Ithird_party/totalsms -Ithird_party/totalsms/scheduler -DSMS_PIXEL_WIDTH=16 -DSMS_LITTLE_ENDIAN=1
PROG_CFLAGS_genesis := -DNDEBUG -Ithird_party/clownmdemu/platform
PROG_CFLAGS_gamegear := $(PROG_CFLAGS_mastersystem)
PROG_CFLAGS_turbografx16 := -fno-strict-aliasing

USER_LIBS := $(OUT)/lib/libui.a $(OUT)/lib/libgfx.a $(OUT)/lib/libmedia.a $(OUT)/lib/libwolfssl.a \
    $(OUT)/lib/libc.a $(OUT)/lib/libfsstack.a $(OUT)/lib/libnetstack.a

# ---- user programs ----------------------------------------------------------

PROG_DIRS := $(wildcard apps/* servers/*)
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

# TLS code is shared by Browser, App Store and the radio audio service.
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
$(OUT)/shared/ld-glaze.so.1: $(OUT)/ldso/entry.o $(OUT)/ldso/ldso.o $(OUT)/ldso/string.o lib/ldso/ldso.lds
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

# ---- assets -----------------------------------------------------------------
# OS artwork and fonts for romfs, generated on macOS by tools/mkassets.swift
# and checked in under assets/gen.

ASSET_FILES := $(wildcard assets/gen/*)
# The browser's bundled pages, romfs "web/", and its trusted roots, "ssl/"
# (tools/mkroots.py).
WEB_FILES := $(wildcard assets/web/*)
SSL_FILES := $(wildcard assets/ssl/*)
# System sounds, romfs "sounds/", synthesized by tools/mksounds.py and checked in.
SOUND_FILES := $(wildcard assets/sounds/*)

# ---- romfs ------------------------------------------------------------------

ROMFS_BINS := $(foreach p,$(PROGS),$(OUT)/romfs/bin/$(p))

$(OUT)/romfs.img: $(ROMFS_LIBRARY_FILES) $(ROMFS_BINS) $(ASSET_FILES) $(WEB_FILES) $(SSL_FILES) $(SOUND_FILES) tools/mkromfs.py
	$(PYTHON) tools/mkromfs.py $@ \
	    $(foreach p,$(PROGS),bin/$(p)=$(OUT)/romfs/bin/$(p)) \
	    $(foreach l,$(ROMFS_LIBRARY_FILES),lib/$(notdir $(l))=$(l)) \
	    $(foreach a,$(ASSET_FILES),res/$(notdir $(a))=$(a)) \
	    $(foreach w,$(WEB_FILES),web/$(notdir $(w))=$(w)) \
	    $(foreach c,$(SSL_FILES),ssl/$(notdir $(c))=$(c)) \
	    $(foreach s,$(SOUND_FILES),sounds/$(notdir $(s))=$(s)) \
	    $(if $(INPUT_SCRIPT),test/input.txt=$(INPUT_SCRIPT))

# ---- kernel -----------------------------------------------------------------

KERNEL_SRCS := $(wildcard kernel/*.c kernel/*.S kernel/drivers/*.c)
KERNEL_OBJS := $(patsubst %,$(OUT)/%.o,$(basename $(KERNEL_SRCS))) $(OUT)/kernel/string.o $(OUT)/kernel/divdi3.o
KERNEL_CFLAGS := $(CFLAGS_COMMON) -O2 -Ikernel -MMD \
    -DGLAZE_BUILD='"$(BUILD_ID)"' -DGLAZE_INIT='"$(INIT)"' \
    $(if $(INPUT_SCRIPT),-DGLAZE_INPUT_SCRIPT)

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

# These bake in INIT/BUILD_ID/INPUT_SCRIPT; rebuild them every time.
$(OUT)/kernel/main.o $(OUT)/kernel/drivers/input.o $(OUT)/romfs.img: FORCE

$(OUT)/kernel.elf: $(KERNEL_OBJS) kernel/kernel.lds
	$(LD) -m elf32ltsmip -T kernel/kernel.lds -o $@ $(KERNEL_OBJS)

# ---- SPL --------------------------------------------------------------------

# The SPL bakes in the kernel's load address, entry and size.
$(OUT)/kernel_consts.flags: $(OUT)/kernel.elf tools/elf_consts.py
	$(PYTHON) tools/elf_consts.py $< > $@

SPL_CFLAGS := $(CFLAGS_COMMON) -Os

$(OUT)/boot/%.o: boot/%.S $(OUT)/kernel_consts.flags
	@mkdir -p $(dir $@)
	$(CC) $(SPL_CFLAGS) $$(cat $(OUT)/kernel_consts.flags) -c -o $@ $<

$(OUT)/boot/%.o: boot/%.c $(OUT)/kernel_consts.flags
	@mkdir -p $(dir $@)
	$(CC) $(SPL_CFLAGS) $$(cat $(OUT)/kernel_consts.flags) -c -o $@ $<

$(OUT)/spl.elf: $(OUT)/boot/spl_start.o $(OUT)/boot/spl.o boot/spl.lds
	$(LD) -m elf32ltsmip -T boot/spl.lds -o $@ $(OUT)/boot/spl_start.o $(OUT)/boot/spl.o

# ---- app packages -------------------------------------------------------------
# Apps that are installed rather than built in (glaze/pkg.h). packages/<name>
# holds the sources, Info and icon.gzi; the build links the app like a built-in
# one, never puts it in romfs or on the factory volume, and packs it as
# out/pkg/<name>.gzp, to be installed from a card (tools/mkcfimg.sh).
# Installed apps run on the dynamic linker, so there are none in a DYNAMIC=0
# build.

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

# ---- factory file system ------------------------------------------------------
# User files the device ships with (userdata/), formatted into the file
# server's region by the same flash stack it runs. Romfs holds only the OS.

FS_BLOCKS := $(shell sed -n 's/^\#define GZ_FLASH_BLOCKS *\([0-9]*\).*/\1/p' lib/include/glaze/abi.h)
USERDATA_FILES := $(shell find userdata -type f ! -name '.*')
MKFSIMG_SRCS := host/mkfsimg.c servers/fs/ftl.c $(FSSTACK_SRCS)

$(OUT)/host/mkfsimg: $(MKFSIMG_SRCS) servers/fs/ftl.h servers/fs/folders.h
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=gnu11 -O2 -Wall -Iservers/fs $(FS_INC) -o $@ $(MKFSIMG_SRCS)

$(OUT)/fs.img: $(OUT)/host/mkfsimg $(USERDATA_FILES) lib/include/glaze/abi.h
	$(OUT)/host/mkfsimg userdata $(FS_BLOCKS) $@

# ---- NAND image -------------------------------------------------------------

# romfs sits raw in NAND after the kernel, not in the kernel image,
# and the kernel pages it in (kernel/romfs.c).
$(OUT)/glaze.bin: $(OUT)/spl.elf $(OUT)/kernel.elf $(OUT)/romfs.img $(OUT)/fs.img tools/mk_nand.py
	$(PYTHON) tools/mk_nand.py --kernel $(OUT)/kernel.elf --spl $(OUT)/spl.elf \
	    --romfs $(OUT)/romfs.img --fs-image $(OUT)/fs.img --out $@

all: $(OUT)/glaze.bin $(PKG_FILES)

# ---- GlazeBoot: boot from RAM out of Windows CE (docs/GLAZEBOOT.md) -----------

# The loader (wince/glazeboot/GlazeBoot.c) is a Windows CE program, built
# on the host with the be300-dev toolchain (clang and the Casio SDK):
# `make glazeboot-exe`. `make glazeboot` builds what it needs from GCC into
# out/glazeboot/gen: the trampoline as bytes (tramp_blob.h) and the layouts
# it shares with the kernel (glazeboot_abi.h). Under ./build.sh the two run
# separately, the first in the container and the second natively.
GB_DIR := $(OUT)/glazeboot
GB_GEN := $(GB_DIR)/gen
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

$(GB_DIR)/romfs.img: $(OUT)/romfs.img
	cp $< $@

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

# RAM-boot test image (tests/run_ramboot.sh): an SPL that hands over the way
# GlazeBoot does, through its trampoline, and optionally a FAT volume
# (RAMBOOT_FS) in the NAND's file-system partition for /NAND.
RB_DIR := $(OUT)/ramboot
RAMBOOT_FS ?=

$(RB_DIR)/%.o: boot/%.S $(OUT)/kernel_consts.flags
	@mkdir -p $(dir $@)
	$(CC) $(SPL_CFLAGS) -DSPL_RAMBOOT $$(cat $(OUT)/kernel_consts.flags) -c -o $@ $<

$(RB_DIR)/%.o: boot/%.c $(OUT)/kernel_consts.flags
	@mkdir -p $(dir $@)
	$(CC) $(SPL_CFLAGS) -DSPL_RAMBOOT $$(cat $(OUT)/kernel_consts.flags) -c -o $@ $<

$(RB_DIR)/spl.elf: $(RB_DIR)/spl_start.o $(RB_DIR)/spl.o $(GB_DIR)/tramp.o $(GB_DIR)/tramp.checked boot/spl.lds
	$(LD) -m elf32ltsmip -T boot/spl.lds -o $@ $(RB_DIR)/spl_start.o $(RB_DIR)/spl.o $(GB_DIR)/tramp.o

$(OUT)/glaze-ramboot.bin: $(RB_DIR)/spl.elf $(OUT)/kernel.elf $(OUT)/romfs.img $(RAMBOOT_FS) tools/mk_nand.py
	$(PYTHON) tools/mk_nand.py --kernel $(OUT)/kernel.elf --spl $(RB_DIR)/spl.elf \
	    --romfs $(OUT)/romfs.img $(if $(RAMBOOT_FS),--fs-image $(RAMBOOT_FS)) --out $@

ramboot: $(OUT)/glaze-ramboot.bin
.PHONY: ramboot

clean:
	rm -rf $(OUT)

FORCE:
.PHONY: all clean packages FORCE

-include $(shell find $(OUT) -name '*.d' 2>/dev/null)

# Portable static store: run ./build.sh store, then publish out/store over HTTPS.
store: packages
	$(PYTHON) tools/mkstore.py --packages $(OUT)/pkg --output $(OUT)/store
.PHONY: store

$(OUT)/include/js_bootstrap.h: apps/browser/js_dom.js tools/mkjsbootstrap.py
	python3 tools/mkjsbootstrap.py $< $@
$(OUT)/apps/browser/script.o $(OUT)/dyn/apps/browser/script.o: $(OUT)/include/js_bootstrap.h
