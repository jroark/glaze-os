# Native build of libgfx for previews and golden-image tests.
CC ?= cc
CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -I../lib/include -I.
GFX := ../lib/gfx/gfx.c ../lib/gfx/blit.c ../lib/gfx/text.c ../lib/gfx/res.c
SB := ../servers/springboard/draw.c ../lib/ui/keypad.c

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

store-catalog-test: ../tests/store_catalog.c ../apps/appstore/catalog.c
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o ../out/host/store-catalog-test $^
	../out/host/store-catalog-test ../out/store/catalog.txt
	python3 ../tests/store_generator.py
.PHONY: store-catalog-test

store-package-test: ../tests/store_packages.c ../lib/libc/pkg.c
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o ../out/host/store-package-test $^
	../out/host/store-package-test
.PHONY: store-package-test

store-http-test: libwolfssl.a
	@mkdir -p ../out/host
	$(CC) $(WEB_CFLAGS) -o ../out/host/store-http-test ../tests/store_http.c \
	    ../apps/browser/http.c ../apps/browser/conn.c ../apps/browser/tls.c ../apps/browser/url.c \
	    ../apps/appstore/inflate.c libwolfssl.a
	python3 ../tests/store_transport.py
.PHONY: store-http-test

# Scalar pixel oracles for clipping, alpha, dithering and three-slice sampling.
gfx-test: ../tests/gfx.c ../lib/gfx/gfx.c ../lib/gfx/blit.c
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -O2 -fsanitize=address,undefined -o ../out/host/gfx-test $^
	../out/host/gfx-test
.PHONY: gfx-test

settings-test: ../tests/settings.c ../tests/settings_sys.h ../apps/settings/main.c
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -O2 -fsanitize=address,undefined -include ../tests/settings_sys.h \
	    -o ../out/host/settings-test ../tests/settings.c hostres.c $(GFX) \
	    ../lib/ui/widgets.c ../lib/ui/keypad.c ../lib/ui/transition.c ../lib/libc/tz.c
	../out/host/settings-test
.PHONY: settings-test

taskmanager-test: ../tests/taskmanager.c ../tests/taskmanager_sys.h ../packages/taskmanager/main.c ../packages/taskmanager/monitor.c
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -O2 -fsanitize=address,undefined -include ../tests/taskmanager_sys.h \
	    -o ../out/host/taskmanager-test ../tests/taskmanager.c ../packages/taskmanager/monitor.c \
	    hostres.c $(GFX) ../lib/ui/widgets.c ../lib/ui/list.c
	../out/host/taskmanager-test
.PHONY: taskmanager-test

ui-events-test: ../tests/ui_events.c ../tests/ui_events_sys.h ../lib/ui/app.c ../lib/ui/list.c ../lib/ui/keyboard.c
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -O2 -fsanitize=address,undefined -include ../tests/ui_events_sys.h \
	    -o ../out/host/ui-events-test ../tests/ui_events.c ../lib/ui/app.c ../lib/ui/list.c \
	    ../lib/ui/keyboard.c ../lib/ui/widgets.c hostres.c $(GFX)
	../out/host/ui-events-test
.PHONY: ui-events-test

aim-test: ../tests/aim.c ../packages/aim/oscar.c ../packages/aim/md5.c ../packages/aim/text.c ../packages/aim/oscar.h
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o ../out/host/aim-test ../tests/aim.c \
	    ../packages/aim/oscar.c ../packages/aim/md5.c ../packages/aim/text.c
	../out/host/aim-test
.PHONY: aim-test

gmail-test: ../tests/gmail.c ../tests/settings_sys.h ../packages/gmail/main.c ../packages/gmail/mime.c ../packages/gmail/imap.c ../packages/gmail/smtp.c ../packages/gmail/mail.h ../packages/gmail/gmail.h libwolfssl.a
	@mkdir -p ../out/host
	$(CC) $(WEB_CFLAGS) -include ../tests/settings_sys.h -o ../out/host/gmail-test ../tests/gmail.c \
	    ../packages/gmail/mime.c ../packages/gmail/imap.c ../packages/gmail/smtp.c \
	    hostres.c $(GFX) ../lib/ui/widgets.c ../lib/ui/list.c ../lib/ui/keyboard.c \
	    ../lib/ui/text.c ../lib/ui/input.c ../lib/ui/transition.c ../lib/libc/tz.c libwolfssl.a
	../out/host/gmail-test
.PHONY: gmail-test

gfx-bench: ../tests/gfx_bench.c ../lib/gfx/gfx.c ../lib/gfx/blit.c ../lib/gfx/text.c
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -O2 -o ../out/host/gfx-bench $^
	../out/host/gfx-bench
.PHONY: gfx-bench

gfx-text-test: ../tests/gfx_text.c ../lib/gfx/gfx.c ../lib/gfx/text.c
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -O2 -fsanitize=address,undefined -o ../out/host/gfx-text-test $^
	../out/host/gfx-text-test ../assets/gen/*.gzf
.PHONY: gfx-text-test

ftl-verify-test: ../tests/ftl_verify.c $(FS)
	@mkdir -p ../out/host
	$(CC) $(FS_CFLAGS) -fsanitize=address,undefined -o ../out/host/ftl-verify-test $^
	../out/host/ftl-verify-test
.PHONY: ftl-verify-test

ftl-verify-bench: ../tests/ftl_verify.c $(FS)
	@mkdir -p ../out/host
	$(CC) $(FS_CFLAGS) -DFTL_BENCH -o ../out/host/ftl-verify-bench $^
	../out/host/ftl-verify-bench
.PHONY: ftl-verify-bench

wince-test: ../tests/wince_pe.c ../apps/wince/pe.c ../apps/wince/pe.h ../tests/wince_datetime.c ../apps/wince/datetime.c ../apps/wince/datetime.h ../tests/wince_ordinals.c ../apps/wince/ordinals.c ../apps/wince/ordinals.h
wince-test: ../tests/wince_registry.c ../apps/wince/registry.c ../apps/wince/registry.h ../tests/wince_dialog.c ../apps/wince/dialog_template.c ../apps/wince/dialog_template.h
wince-test: ../tests/wince_property.c ../apps/wince/property_state.c ../apps/wince/property_state.h ../apps/wince/number.c ../apps/wince/number.h
	@mkdir -p ../out/host
	$(CC) -std=c11 -g -Wall -Wextra -I../lib/include -fsanitize=address,undefined \
	    -o ../out/host/wince-pe-test ../tests/wince_pe.c ../apps/wince/pe.c
	../out/host/wince-pe-test
	$(CC) -std=c11 -g -Wall -Wextra -I../lib/include -fsanitize=address,undefined \
	    -o ../out/host/wince-datetime-test ../tests/wince_datetime.c ../apps/wince/datetime.c
	../out/host/wince-datetime-test
	$(CC) -std=c11 -g -Wall -Wextra -I../lib/include -fsanitize=address,undefined \
	    -o ../out/host/wince-ordinals-test ../tests/wince_ordinals.c ../apps/wince/ordinals.c
	../out/host/wince-ordinals-test
	$(CC) -std=c11 -g -Wall -Wextra -I../lib/include -fsanitize=address,undefined \
	    -o ../out/host/wince-registry-test ../tests/wince_registry.c ../apps/wince/registry.c ../apps/wince/pe.c
	../out/host/wince-registry-test
	$(CC) -std=c11 -g -Wall -Wextra -I../lib/include -fsanitize=address,undefined \
	    -o ../out/host/wince-dialog-test ../tests/wince_dialog.c ../apps/wince/dialog_template.c ../apps/wince/pe.c
	../out/host/wince-dialog-test
	$(CC) -std=c11 -g -Wall -Wextra -I../lib/include -fsanitize=address,undefined \
	    -o ../out/host/wince-property-test ../tests/wince_property.c ../apps/wince/property_state.c ../apps/wince/number.c
	../out/host/wince-property-test
	python3 ../tests/wince_assets_test.py
.PHONY: wince-test

../out/host/wince-fs-read: ../tests/wince_fs_read.c $(FS)
	@mkdir -p ../out/host
	$(CC) $(FS_CFLAGS) -o $@ $^
