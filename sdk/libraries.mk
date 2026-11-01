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
$(OUT)/libc/entropy_jent.o: PIC += -O0 -Ithird_party/jitterentropy
LDFLAGS := -m elf32ltsmip --hash-style=sysv -z now -z relro -z text -z max-page-size=4096 -z separate-code
LIBS := c gfx ui media
LIBFILES := $(foreach n,$(LIBS),$(OUT)/lib$(n).so.1) $(OUT)/libwolfssl.so.1
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
# wolfSSL, as Glaze builds it (third-party code, so without Glaze's warnings).
# Its glaze_tls_seed/glaze_tls_time imports come from each executable, so no -z defs.
WOLFSSL_SRCS := $(addprefix third_party/wolfssl/,src/ssl.c src/internal.c src/tls.c src/tls13.c \
    src/keys.c src/wolfio.c $(addprefix wolfcrypt/src/,aes.c sha.c sha256.c sha512.c hmac.c hash.c \
    kdf.c random.c rsa.c ecc.c curve25519.c fe_low_mem.c chacha.c poly1305.c chacha20_poly1305.c \
    sp_int.c wolfmath.c asn.c coding.c memory.c wc_port.c wc_encrypt.c error.c logging.c))
WOLFSSL_CFLAGS := $(ARCH) -mabicalls -fPIC -std=gnu11 -ffreestanding -fno-builtin -nostdlib \
    -fno-stack-protector -Os -ffunction-sections -fdata-sections -MMD -g -Ilib/include -Ilib/libc/include \
    -Ithird_party/wolfssl -Ilib/wolfssl -DWOLFSSL_USER_SETTINGS
$(OUT)/wolfssl/%.o: third_party/wolfssl/%.c
	@mkdir -p $(dir $@)
	$(CC) $(WOLFSSL_CFLAGS) -c $< -o $@
$(OUT)/libwolfssl.so.1: $(patsubst third_party/wolfssl/%.c,$(OUT)/wolfssl/%.o,$(WOLFSSL_SRCS)) lib/wolfssl/exports.map $(OUT)/libc.so.1
	$(LD) $(LDFLAGS) -shared -soname libwolfssl.so.1 --gc-sections --version-script=lib/wolfssl/exports.map \
	    -o $@ $(filter %.o,$^) -L$(OUT) --as-needed -l:libc.so.1
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
