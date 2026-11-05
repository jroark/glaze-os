# ---- userspace libraries ----------------------------------------------------

USER_CFLAGS := $(CFLAGS_COMMON) -O2 -ffunction-sections -fdata-sections -MMD
PIC_CFLAGS := $(filter-out -mno-abicalls -fno-pic,$(CFLAGS_COMMON)) -mabicalls -fPIC -O2 -ffunction-sections -fdata-sections -MMD
DYN_LDFLAGS := -m elf32ltsmip --hash-style=sysv -z now -z relro -z text -z max-page-size=4096 -z separate-code

LIBC_SRCS := $(wildcard lib/libc/*.c)
$(OUT)/lib/libc/entropy_jent.o: USER_CFLAGS += -O0 -Ithird_party/jitterentropy
$(OUT)/pic/libc/entropy_jent.o: PIC_CFLAGS += -O0 -Ithird_party/jitterentropy
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
NET_INC := -Ithird_party/lwip/src/include -Iservers/net -Ithird_party/hostap/src \
    -Ithird_party/hostap/src/utils -include servers/net/wpa/config.h -DGLAZE_HOSTAP
NETSTACK_SRCS := $(wildcard third_party/lwip/src/core/*.c third_party/lwip/src/core/ipv4/*.c \
    third_party/lwip/src/api/*.c) \
    third_party/lwip/src/netif/ethernet.c \
    $(wildcard third_party/lwip/src/netif/ppp/*.c third_party/lwip/src/netif/ppp/polarssl/*.c)
HOSTAP_SRCS := $(shell find third_party/hostap/src -name '*.c')
NETSTACK_SRCS += $(HOSTAP_SRCS)
$(patsubst %.c,$(OUT)/%.o,$(HOSTAP_SRCS)): TP_INC += $(NET_INC)
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


PROG_CFLAGS_usb := $(WOLFSSL_INC)
PROG_DEPS_usb := $(OUT)/shared/libwolfssl.so.1
PROG_LDLIBS_usb := -l:libwolfssl.so.1
$(OUT)/servers/usb/%.o: USER_CFLAGS += $(WOLFSSL_INC)

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
$(patsubst %.c,$(OUT)/dyn/%.o,$(wildcard apps/wince/*.c)): Makefile mk/programs.mk
PROG_DEPS_video := $(OUT)/shared/libmedia.so.1
PROG_LDLIBS_video := -l:libmedia.so.1
PROG_DEPS_audio := $(OUT)/pic/libhelix.a $(OUT)/shared/libwolfssl.so.1 $(OUT)/shared/libmedia.so.1
PROG_LDLIBS_audio := $(OUT)/pic/libhelix.a -l:libwolfssl.so.1 -l:libmedia.so.1
PROG_DEPS_browser := $(OUT)/shared/libwolfssl.so.1 $(OUT)/pic/libjerry.a
PROG_LDLIBS_browser := -l:libwolfssl.so.1 $(OUT)/pic/libjerry.a
PROG_STATIC_DEPS_browser := $(OUT)/lib/libjerry.a
PROG_LDFLAGS_browser := --exclude-libs ALL
# InfoNES is portable C despite upstream's historical .cpp filenames.

USER_LIBS := $(OUT)/lib/libui.a $(OUT)/lib/libgfx.a $(OUT)/lib/libmedia.a $(OUT)/lib/libwolfssl.a \
    $(OUT)/lib/libc.a $(OUT)/lib/libfsstack.a $(OUT)/lib/libnetstack.a
