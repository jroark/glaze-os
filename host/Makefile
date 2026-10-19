# Native build of libgfx for previews and golden-image tests.
CC ?= cc
CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -I../lib/include -I.
GFX := ../lib/gfx/gfx.c ../lib/gfx/blit.c ../lib/gfx/text.c ../lib/gfx/res.c
SB := ../servers/springboard/draw.c

# The file server's flash stack, against a fault-injecting RAM NAND.
FS := ../servers/fs/ftl.c ../third_party/littlefs/lfs.c ../third_party/littlefs/lfs_util.c \
    ../third_party/dhara/dhara/map.c ../third_party/dhara/dhara/journal.c \
    ../third_party/dhara/dhara/error.c ../third_party/dhara/ecc/bch.c ../third_party/dhara/ecc/gf13.c
FS_CFLAGS := -std=gnu11 -O2 -g -Wall -I../servers/fs -I../third_party/littlefs -I../third_party/dhara \
    -DLFS_NO_DEBUG -DLFS_NO_WARN -DLFS_NO_ERROR

# The file server's CompactFlash volume (FatFs), against an image file.
FAT := ../servers/fs/card.c ../third_party/fatfs/ff.c ../third_party/fatfs/ffunicode.c
FAT_CFLAGS := $(CFLAGS) -I../servers/fs -I../third_party/fatfs -fsanitize=address,undefined

# The browser engine, rendering pages to BMPs, with wolfSSL for https.
WEB := $(filter-out ../apps/browser/main.c,$(wildcard ../apps/browser/*.c))
WEB_CFLAGS := $(CFLAGS) -I../third_party/stb -I../third_party/wolfssl -I../apps/browser/tls \
    -DWOLFSSL_USER_SETTINGS -fsanitize=address,undefined
WOLFSSL := $(addprefix ../third_party/wolfssl/,src/ssl.c src/internal.c src/tls.c src/tls13.c src/keys.c \
    src/wolfio.c $(addprefix wolfcrypt/src/,aes.c sha.c sha256.c sha512.c hmac.c hash.c kdf.c random.c \
    rsa.c ecc.c curve25519.c fe_low_mem.c chacha.c poly1305.c chacha20_poly1305.c sp_int.c wolfmath.c \
    asn.c coding.c memory.c wc_port.c wc_encrypt.c error.c logging.c))
WOLFSSL_CFLAGS := -std=gnu11 -O2 -g -I../third_party/wolfssl -I../apps/browser/tls -DWOLFSSL_USER_SETTINGS

# libmedia and the Helix MP3 decoder (the audio server's and Video app's
# decoding), compared against ffmpeg by tests/mediatest.py.
MEDIA := $(wildcard ../lib/media/*.c ../third_party/helix-mp3/*.c ../third_party/helix-mp3/real/*.c)
MEDIA_CFLAGS := $(CFLAGS) -Wno-unused-but-set-variable -I../third_party/helix-mp3/pub -I../third_party/stb \
    -fsanitize=address,undefined -fno-sanitize=shift

all: sheet preview fstest fattest mkfsimg browse mediatest

sheet: sheet.c hostres.c $(GFX)
	$(CC) $(CFLAGS) -o $@ $^

preview: preview.c hostres.c $(GFX) $(SB)
	$(CC) $(CFLAGS) -o $@ $^

shots: preview
	./preview
	for f in lock lock_drag unlock_anim home home_dusk_pressed home_badges home_banner lock_notes; do sips -s format png $$f.bmp --out $$f.png >/dev/null; done

fstest: fstest.c $(FS)
	$(CC) $(FS_CFLAGS) -o $@ $^

fattest: fattest.c $(FAT) ../servers/fs/card.h
	$(CC) $(FAT_CFLAGS) -o $@ fattest.c $(FAT)

# fattest on fresh FAT12, FAT16 and FAT32 images, each checked with fsck.fat
# afterwards (needs dosfstools and mtools).
fat-test: fattest
	set -e; for f in "12 4" "16 16" "32 64"; do set -- $$f; \
	    ../tools/mkcfimg.sh fat$$1.img $$2 $$1 ../userdata/Documents >/dev/null; \
	    ./fattest fat$$1.img >fat$$1.log || { grep FAIL fat$$1.log; exit 1; }; \
	    fsck.fat -n fat$$1.img >/dev/null; echo "FAT$$1: fattest and fsck.fat pass"; \
	    rm -f fat$$1.img fat$$1.log; done
.PHONY: fat-test

# The top-level Makefile builds its own copy into out/host for the image.
mkfsimg: mkfsimg.c $(FS)
	$(CC) $(FS_CFLAGS) -o $@ $^

# wolfSSL is built once, without sanitizers (its constant-time code trips UBSan's shift checks).
WOLFSSL_OBJS := $(patsubst ../third_party/wolfssl/%.c,wolfssl.o/%.o,$(WOLFSSL))
wolfssl.o/%.o: ../third_party/wolfssl/%.c ../apps/browser/tls/user_settings.h
	@mkdir -p $(dir $@)
	$(CC) $(WOLFSSL_CFLAGS) -c -o $@ $<
libwolfssl.a: $(WOLFSSL_OBJS)
	rm -f $@
	ar rcs $@ $^

browse: browse.c hostres.c $(GFX) $(WEB) ../apps/browser/web.h libwolfssl.a
	$(CC) $(WEB_CFLAGS) -o $@ browse.c hostres.c $(GFX) $(WEB) libwolfssl.a

mediatest: mediatest.c $(MEDIA)
	$(CC) $(MEDIA_CFLAGS) -o $@ $^ -lm

clean:
	rm -rf wolfssl.o
	rm -f sheet preview fstest fattest mkfsimg browse libwolfssl.a mediatest stowaway-test elf-test *.bmp *.png

# Byte decoder and shared text editing, with ASan/UBSan.
stowaway-test: ../tests/stowaway.c ../kernel/drivers/stowaway.c ../lib/ui/input.c
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o $@ $^
	./$@
.PHONY: stowaway-test

elf-test: ../tests/elf.c ../lib/include/glaze/elf.h
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o $@ ../tests/elf.c
	./$@
.PHONY: elf-test
