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
FAT := ../servers/fs/card.c ../servers/fs/nanddisk.c ../third_party/fatfs/ff.c ../third_party/fatfs/ffunicode.c
FAT_CFLAGS := $(CFLAGS) -I../servers/fs -I../third_party/fatfs -fsanitize=address,undefined

# The browser engine, rendering pages to BMPs, with wolfSSL for https.
WEB := $(filter-out ../apps/browser/main.c,$(wildcard ../apps/browser/*.c))
WEB_CFLAGS := $(CFLAGS) -I../third_party/stb -I../third_party/wolfssl -I../apps/browser/tls \
    -DWOLFSSL_USER_SETTINGS -I../third_party/jerryscript -I../apps/browser/js_platform -I../out/include \
    -include ../apps/browser/js_config.h -fsanitize=address,undefined
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
# afterwards (needs dosfstools and mtools). The FAT16 run also uses a 10 MiB
# FAT16 volume (mkfs.fat's FAT16 needs 9) as /NAND, in Windows CE's NAND
# layout (nanddisk.c).
fat-test: fattest
	set -e; for f in "12 4" "16 16" "32 64"; do set -- $$f; \
	    ../tools/mkcfimg.sh fat$$1.img $$2 $$1 ../userdata/Documents >/dev/null; \
	    nand=; [ $$1 = 16 ] && { ../tools/mkcfimg.sh nand16.img 10 16 ../userdata/Documents >/dev/null; nand=nand16.img; }; \
	    ./fattest fat$$1.img $$nand >fat$$1.log || { grep FAIL fat$$1.log; exit 1; }; \
	    fsck.fat -n fat$$1.img >/dev/null; [ -z "$$nand" ] || fsck.fat -n $$nand >/dev/null; \
	    echo "FAT$$1: fattest and fsck.fat pass$${nand:+ (with /NAND)}"; \
	    rm -f fat$$1.img fat$$1.log $$nand; done
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

WEB_OBJS := $(patsubst ../apps/browser/%.c,browser.o/%.o,$(WEB))
browser.o/%.o: ../apps/browser/%.c
	@mkdir -p $(dir $@)
	$(CC) $(WEB_CFLAGS) -MMD -MP -c -o $@ $<
browser.o/script.o: ../out/include/js_bootstrap.h
-include $(WEB_OBJS:.o=.d)

browse: browse.c hostres.c $(GFX) $(WEB_OBJS) $(wildcard ../apps/browser/*.h ../apps/browser/js_platform/*) libwolfssl.a libjerry.a ../out/include/js_bootstrap.h
	$(CC) $(WEB_CFLAGS) -o $@ browse.c hostres.c $(GFX) $(WEB_OBJS) libwolfssl.a libjerry.a -lm

mediatest: mediatest.c $(MEDIA)
	$(CC) $(MEDIA_CFLAGS) -o $@ $^ -lm

clean:
	rm -rf wolfssl.o browser.o
	rm -f jerry.o libjerry.a
	rm -f sheet preview fstest fattest mkfsimg browse libwolfssl.a mediatest stowaway-test elf-test *.bmp *.png

# Byte decoder and shared text editing, with ASan/UBSan.
# The NAND ECC decoder ported from Windows CE's nanddisk.dll (RAM boot /NAND).
nand-ecc-test: ../tests/nand_ecc.c ../kernel/drivers/nand_ecc.c ../kernel/drivers/nand_ecc.h
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o $@ $(filter %.c,$^)
	./$@
.PHONY: nand-ecc-test

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

radio-test: ../tests/radio_protocol.c ../servers/audio/radio_protocol.c ../servers/audio/radio_mp3.c
	@mkdir -p ../out/host
	$(CC) $(MEDIA_CFLAGS) -o ../out/host/radio-test $^ $(wildcard ../third_party/helix-mp3/*.c ../third_party/helix-mp3/real/*.c)
	../out/host/radio-test ../userdata/Music/Demo.mp3
.PHONY: radio-test

radio-worker-test: libwolfssl.a
	@mkdir -p ../out/host
	$(CC) $(MEDIA_CFLAGS) -I../apps/browser -I../third_party/wolfssl -I../apps/browser/tls \
	    -DWOLFSSL_USER_SETTINGS -include ../tests/radio_host.h -o ../out/host/radio-worker-test \
	    ../tests/radio_worker.c ../servers/audio/radio.c ../servers/audio/radio_protocol.c ../servers/audio/radio_mp3.c \
	    ../apps/browser/conn.c ../apps/browser/tls.c ../apps/browser/url.c \
	    $(wildcard ../third_party/helix-mp3/*.c ../third_party/helix-mp3/real/*.c) libwolfssl.a -lpthread
	python3 ../tests/radio_transport.py
.PHONY: radio-worker-test

somafm-ui-test: ../tests/somafm_ui.c ../tests/somafm_sys.h ../packages/somafm/main.c ../packages/somafm/stations.h
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -fsanitize=address,undefined -include ../tests/somafm_sys.h \
	    -o ../out/host/somafm-ui-test ../tests/somafm_ui.c hostres.c $(GFX) \
	    ../lib/ui/widgets.c ../lib/ui/list.c ../lib/ui/text.c
	../out/host/somafm-ui-test
.PHONY: somafm-ui-test

# Scalar pixel oracles for clipping, alpha, dithering and three-slice sampling.
gfx-test: ../tests/gfx.c ../lib/gfx/gfx.c ../lib/gfx/blit.c
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -O2 -fsanitize=address,undefined -o ../out/host/gfx-test $^
	../out/host/gfx-test
.PHONY: gfx-test

settings-test: ../tests/settings.c ../tests/settings_sys.h ../apps/settings/main.c ../lib/libc/touch.c
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -O2 -fsanitize=address,undefined -include ../tests/settings_sys.h \
	    -o ../out/host/settings-test ../tests/settings.c hostres.c $(GFX) \
	    ../lib/ui/widgets.c ../lib/ui/keypad.c ../lib/ui/transition.c ../lib/libc/tz.c ../lib/libc/touch.c
	../out/host/settings-test
.PHONY: settings-test

touch-calibration-test: ../tests/touch_calibration.c ../lib/libc/touch.c ../servers/springboard/calibration.c
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -O2 -fsanitize=address,undefined -o ../out/host/touch-calibration-test $^
	../out/host/touch-calibration-test
.PHONY: touch-calibration-test

display-test: ../tests/display.c ../tests/display_sys.h ../kernel/drivers/display.c ../servers/springboard/display.c
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -O2 -fsanitize=address,undefined -include ../tests/display_sys.h \
	    -o ../out/host/display-test ../tests/display.c ../servers/springboard/display.c
	../out/host/display-test
.PHONY: display-test

idle-test: ../tests/idle.c ../servers/springboard/idle.c ../servers/springboard/idle.h ../lib/include/glaze/display.h
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -O2 -fsanitize=address,undefined -o ../out/host/idle-test ../tests/idle.c ../servers/springboard/idle.c
	../out/host/idle-test
.PHONY: idle-test

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
wince-test: ../tests/wince_text.c ../apps/wince/text.c ../apps/wince/text.h
wince-test: ../tests/wince_runtime.c ../tests/wince_runtime_host.h ../tests/wince_filemeta.c ../apps/wince/filemeta.c ../apps/wince/thread.c ../apps/wince/mapping.c ../apps/wince/timezone.c ../apps/wince/scan.c ../apps/wince/string.c ../apps/wince/view.c ../apps/wince/winsock.c ../apps/wince/ole.c
wince-test: ../apps/wince/list.c ../apps/wince/date_control.c ../apps/wince/tabs.c
wince-test: ../apps/wince/database.c ../tests/wince_database.c
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
	$(CC) -std=c11 -g -Wall -Wextra -I../lib/include -fsanitize=address,undefined \
	    -o ../out/host/wince-text-test ../tests/wince_text.c ../apps/wince/text.c
	../out/host/wince-text-test
	$(CC) -std=c11 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -I../lib/include \
	    -fsanitize=address,undefined -pthread -o ../out/host/wince-runtime-test ../tests/wince_runtime.c ../apps/wince/pe.c ../apps/wince/datetime.c ../lib/libc/tz.c
	../out/host/wince-runtime-test
	$(CC) -std=c11 -g -Wall -Wextra -I../lib/include -fsanitize=address,undefined -D_DEFAULT_SOURCE \
	    -o ../out/host/wince-filemeta-test ../tests/wince_filemeta.c ../apps/wince/datetime.c ../apps/wince/pe.c
	../out/host/wince-filemeta-test
	$(CC) -std=c11 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -I../lib/include -fsanitize=address,undefined \
	    -o ../out/host/wince-database-test ../tests/wince_database.c ../apps/wince/pe.c
	../out/host/wince-database-test
	python3 ../tests/wince_assets_test.py
	python3 ../tests/wince_runner_test.py
.PHONY: wince-test

# GlazeBoot's hand-over plan (wince/glazeboot/plan.h) on a RAM model.
glazeboot-test: ../tests/glazeboot_plan.c ../wince/glazeboot/plan.h
	@mkdir -p ../out/host
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -fsanitize=address,undefined \
	    -o ../out/host/glazeboot-plan-test ../tests/glazeboot_plan.c
	../out/host/glazeboot-plan-test
.PHONY: glazeboot-test

../out/host/wince-fs-read: ../tests/wince_fs_read.c $(FS)
	@mkdir -p ../out/host
	$(CC) $(FS_CFLAGS) -o $@ $^

# NES portable core plus original homebrew cartridge, with memory/UB checking.
NES := $(filter-out ../packages/nes/main.c,$(wildcard ../packages/nes/*.c))
nes-test: ../out/pkg-res/nes/demo.nes
	@mkdir -p ../out/host
	$(CC) -std=gnu11 -O2 -g -fno-strict-aliasing -fsanitize=address,undefined \
	    -o ../out/host/nes-test ../tests/nes.c $(NES)
	../out/host/nes-test ../out/pkg-res/nes/demo.nes
.PHONY: nes-test

nes-cpu-test:
	@mkdir -p ../out/host
	$(CC) -std=gnu11 -O2 -g -fno-strict-aliasing -fsanitize=address,undefined \
	    -o ../out/host/nes-trace ../tests/nes_trace.c $(NES)
	../out/host/nes-trace "$(NES_TEST_ROM)" "$(NES_TEST_LOG)"
.PHONY: nes-cpu-test

# Game Boy portable adapter and original DMG/CGB cartridges.
GAMEBOY := $(filter-out ../packages/gameboy/main.c ../packages/gameboy/save.c,$(wildcard ../packages/gameboy/*.c))
gameboy-test: gameboy-save-test ../out/pkg-res/gameboy/demo.gb ../out/pkg-res/gameboy/demo.gbc
	@mkdir -p ../out/host
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing -DIS_LITTLE_ENDIAN -fsanitize=address,undefined \
	    -o ../out/host/gameboy-test ../tests/gameboy.c $(GAMEBOY)
	../out/host/gameboy-test ../out/pkg-res/gameboy/demo.gb ../out/pkg-res/gameboy/demo.gbc
.PHONY: gameboy-test

gameboy-rom-test:
	@mkdir -p ../out/host
	$(CC) -std=gnu11 -O2 -g -fno-strict-aliasing -DIS_LITTLE_ENDIAN -fsanitize=address,undefined \
	    -o ../out/host/gameboy-rom ../tests/gameboy_rom.c $(GAMEBOY)
	../out/host/gameboy-rom "$(GB_TEST_ROM)"
.PHONY: gameboy-rom-test


gameboy-save-test:
	@mkdir -p ../out/host
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -I../lib/include -fsanitize=address,undefined \
	    -o ../out/host/gameboy-save ../tests/gameboy_save.c ../packages/gameboy/save.c
	../out/host/gameboy-save
.PHONY: gameboy-save-test


../out/pkg-res/nes/demo.nes: ../tools/mknesdemo.py
	python3 $< --output $@
../out/pkg-res/gameboy/demo.gb: ../tools/mkgbdemo.py
	python3 $< --output-dir $(dir $@) --mode dmg
../out/pkg-res/gameboy/demo.gbc: ../tools/mkgbdemo.py
	python3 $< --output-dir $(dir $@) --mode cgb

# Sega adapters use original cartridges generated only into ignored output.
SMS_CORE := $(filter-out ../packages/mastersystem/main.c ../packages/mastersystem/save.c,$(wildcard ../packages/mastersystem/*.c))
GENESIS_CORE := $(filter-out ../packages/genesis/main.c ../packages/genesis/save.c,$(wildcard ../packages/genesis/*.c))
SMS_FLAGS := -I../third_party/totalsms -I../third_party/totalsms/scheduler -DSMS_PIXEL_WIDTH=16 -DSMS_LITTLE_ENDIAN=1
SEGA_TEST_FLAGS := -std=gnu11 -O2 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined -fno-sanitize-recover=all
mastersystem-test: ../out/pkg-res/mastersystem/demo.sms
	@mkdir -p ../out/host
	$(CC) $(SEGA_TEST_FLAGS) $(SMS_FLAGS) -o ../out/host/sms-test ../tests/sega.c $(SMS_CORE)
	../out/host/sms-test $< ../out/host/sms.ppm
genesis-test: ../out/pkg-res/genesis/demo.gen
	@mkdir -p ../out/host
	$(CC) $(SEGA_TEST_FLAGS) -DTEST_GENESIS -o ../out/host/genesis-test ../tests/sega.c $(GENESIS_CORE)
	../out/host/genesis-test $< ../out/host/genesis.ppm
../out/pkg-res/mastersystem/demo.sms: ../tools/mksegademo.py
	python3 $< sms --output $@
../out/pkg-res/genesis/demo.gen: ../tools/mksegademo.py
	python3 $< genesis --output $@
.PHONY: mastersystem-test genesis-test

sms-banks-test:
	@mkdir -p ../out/host
	$(CC) $(SEGA_TEST_FLAGS) $(SMS_FLAGS) -o ../out/host/sms-banks ../tests/sms_banks.c $(SMS_CORE) ../tests/sega_audio_stub.c
	../out/host/sms-banks
.PHONY: sms-banks-test

sega-rom-runners:
	@mkdir -p ../out/host
	$(CC) $(SEGA_TEST_FLAGS) $(SMS_FLAGS) -o ../out/host/sms-rom ../tests/sega_rom.c $(SMS_CORE)
	$(CC) $(SEGA_TEST_FLAGS) -o ../out/host/genesis-rom ../tests/sega_rom.c $(GENESIS_CORE)
.PHONY: sega-rom-runners

GG_CORE := $(filter-out ../packages/gamegear/main.c ../packages/gamegear/save.c,$(wildcard ../packages/gamegear/*.c))
PCE_CORE := $(filter-out ../packages/turbografx16/main.c ../packages/turbografx16/save.c,$(wildcard ../packages/turbografx16/*.c))
gamegear-test: ../out/pkg-res/gamegear/demo.gg
	@mkdir -p ../out/host
	$(CC) $(SEGA_TEST_FLAGS) $(SMS_FLAGS) -DTEST_GG -o ../out/host/gamegear-test ../tests/console.c $(GG_CORE)
	../out/host/gamegear-test $< ../out/host/gamegear.ppm
turbografx16-test: ../out/pkg-res/turbografx16/demo.pce
	@mkdir -p ../out/host
	$(CC) $(SEGA_TEST_FLAGS) -fno-strict-aliasing -o ../out/host/turbografx16-test ../tests/console.c $(PCE_CORE)
	../out/host/turbografx16-test $< ../out/host/turbografx16.ppm
../out/pkg-res/gamegear/demo.gg: ../tools/mksegademo.py
	python3 $< gamegear --output $@
../out/pkg-res/turbografx16/demo.pce: ../tools/mkpcedemo.py
	python3 $< --output $@
.PHONY: gamegear-test turbografx16-test

gamegear-hardware-test: ../out/pkg-res/gamegear/demo.gg
	@mkdir -p ../out/host
	$(CC) $(SEGA_TEST_FLAGS) $(SMS_FLAGS) -o ../out/host/gamegear-hardware ../tests/gamegear_hardware.c $(GG_CORE) ../tests/sega_audio_stub.c
	../out/host/gamegear-hardware $<
pce-hardware-test:
	@mkdir -p ../out/host
	$(CC) $(SEGA_TEST_FLAGS) -fno-strict-aliasing -o ../out/host/pce-hardware ../tests/pce_hardware.c $(PCE_CORE)
	../out/host/pce-hardware
.PHONY: gamegear-hardware-test pce-hardware-test

# Keep the engine private to Browser. Native tests use the same feature profile.
jerry.o: ../third_party/jerryscript/jerryscript.c ../third_party/jerryscript/jerryscript.h ../apps/browser/js_config.h
	$(CC) -std=c99 -O2 -g -I../third_party/jerryscript -include ../apps/browser/js_config.h -c -o $@ $<
libjerry.a: jerry.o
	ar rcs $@ $^
../out/include/js_bootstrap.h: ../apps/browser/js_dom.js ../tools/mkjsbootstrap.py
	python3 ../tools/mkjsbootstrap.py $< $@

../out/host/browser-platform.o: browse.c
	@mkdir -p ../out/host
	$(CC) $(WEB_CFLAGS) -Dmain=browser_preview_main -c -o $@ $<
browser-test: $(WEB_OBJS) libwolfssl.a libjerry.a ../out/host/browser-platform.o
	$(CC) $(WEB_CFLAGS) -o ../out/host/browser-test ../tests/browser_script.c \
	    ../out/host/browser-platform.o hostres.c $(GFX) $(WEB_OBJS) libwolfssl.a libjerry.a -lm
	python3 ../tests/browser_transport.py ../out/host/browser-test
.PHONY: browser-test

# Opt-in live Google Search workflow; build only, never run as part of CI.
browser-google: $(WEB_OBJS) libwolfssl.a libjerry.a ../out/host/browser-platform.o
	$(CC) $(WEB_CFLAGS) -o ../out/host/browser-google ../tests/browser_google.c \
	    ../out/host/browser-platform.o hostres.c $(GFX) $(WEB_OBJS) libwolfssl.a libjerry.a -lm
.PHONY: browser-google
