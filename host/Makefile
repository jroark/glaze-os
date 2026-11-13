# Native build of libgfx for previews and golden-image tests.
CC ?= cc
CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -I../lib/include -I.
GFX := ../lib/gfx/gfx.c ../lib/gfx/blit.c ../lib/gfx/text.c ../lib/gfx/res.c
SB := ../servers/springboard/draw.c ../servers/springboard/layout.c ../lib/ui/keypad.c
EDIT := ../lib/ui/edit.c clipboard_stub.c
KEYBOARD := ../lib/ui/keyboard.c ../lib/ui/handwriting.c ../lib/ui/recognize.c $(EDIT)

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

# Every binary, object and render goes under out/host, never into the source tree.
O := ../out/host
$(shell mkdir -p $(O))

all: sheet preview fstest fattest mkfsimg browse mediatest

clipboard-test: ../tests/native/clipboard.c ../tests/native/clipboard_sys.h ../lib/ui/clipboard.c ../lib/ui/edit.c ../servers/springboard/clipboard.c
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -O2 -fsanitize=address,undefined -include ../tests/native/clipboard_sys.h \
	    $(filter %.c,$^) -o ../out/host/clipboard-test
	../out/host/clipboard-test
.PHONY: clipboard-test

# Native USB development CLI. Prefer the Apple Silicon Homebrew installation.
USB_PKG_CONFIG ?= $(if $(wildcard /opt/homebrew/bin/pkg-config),/opt/homebrew/bin/pkg-config,pkg-config)
USB_FLAGS := $(shell $(USB_PKG_CONFIG) --cflags --libs libusb-1.0 2>/dev/null)
USB_CC ?= $(CC)
USB_SOURCES := $(wildcard ../tools/glaze-usb/*.c) ../lib/media/veo_jpeg.c ../lib/media/jpeg.c
USB_TLS := -I../third_party/wolfssl -I../apps/browser/tls -DWOLFSSL_USER_SETTINGS
../out/host/glaze-usb: $(USB_SOURCES) $(wildcard ../tools/glaze-usb/*.h) ../lib/include/glaze/usb_wire.h ../lib/include/glaze/usb_boot.h ../lib/include/glaze/abi.h ../lib/include/glaze/debug_net.h ../lib/include/glaze/usb_net.h ../lib/include/glaze/veo.h ../lib/include/glaze/media.h $(O)/libwolfssl.a
	@mkdir -p ../out/host
	$(USB_CC) -std=gnu11 -O2 -g -Wall -Wextra -I../lib/include $(USB_TLS) $(USB_SOURCES) $(O)/libwolfssl.a $(USB_FLAGS) -o $@
glaze-usb: ../out/host/glaze-usb
.PHONY: glaze-usb

usb-wire-test: ../tests/native/usb_wire.c ../lib/include/glaze/usb_wire.h ../lib/include/glaze/usb_boot.h
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -fsanitize=address,undefined ../tests/native/usb_wire.c -o ../out/host/usb-wire-test
	../out/host/usb-wire-test
usb-driver-test: ../tests/native/usb_driver.c ../kernel/drivers/usb.c
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -Wno-pointer-to-int-cast -fsanitize=address,undefined -I$(BE300_SOURCE)/src/hw ../tests/native/usb_driver.c $(BE300_SOURCE)/src/hw/usbf.c -o ../out/host/usb-driver-test
	../out/host/usb-driver-test
.PHONY: usb-wire-test usb-driver-test
.PHONY: usb-net-test
.PHONY: mirror-test
../out/host/mirror-fixture: ../tests/native/mirror_fixture.c ../tools/glaze-usb/mirror.c ../tools/glaze-usb/rfb.c ../servers/usb/mirror.c ../tools/glaze-usb/rfb.h ../servers/usb/mirror.h ../lib/include/glaze/mirror.h
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) $(shell $(USB_PKG_CONFIG) --cflags libusb-1.0) -fsanitize=address,undefined $(filter %.c,$^) -o $@
mirror-test: ../out/host/mirror-fixture
	$(CC) $(CFLAGS) -fsanitize=address,undefined ../tests/native/mirror_frame.c ../servers/usb/mirror.c -o ../out/host/mirror-frame
	../out/host/mirror-frame
	python3 ../tests/mirror.py
usb-net-test: ../tests/native/usb_ethernet.c ../servers/usb/ethernet.c ../tools/glaze-usb/ethernet.c ../tools/glaze-usb/ethernet_packet.c ../lib/include/glaze/usb_net.h ../servers/usb/ethernet.h ../tools/glaze-usb/ethernet_packet.h
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) $(shell $(USB_PKG_CONFIG) --cflags libusb-1.0) -fsanitize=address,undefined $(filter %.c,$^) -o ../out/host/usb-net-test
	../out/host/usb-net-test
usb-reload-test: ../tests/native/usb_reload.c ../kernel/usb_reload.c ../out/glazeboot/gen/tramp_blob.h
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -I../out/glazeboot/gen -fsanitize=address,undefined ../tests/native/usb_reload.c -o ../out/host/usb-reload-test
	../out/host/usb-reload-test
.PHONY: usb-reload-test
usb-pcconnect-test: ../tests/native/usb_pcconnect.c ../tools/glaze-usb/pcconnect.c ../tools/glaze-usb/pcconnect.h
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -fsanitize=address,undefined ../tests/native/usb_pcconnect.c ../tools/glaze-usb/pcconnect.c $(USB_FLAGS) -o ../out/host/usb-pcconnect-test
	../out/host/usb-pcconnect-test
.PHONY: usb-pcconnect-test
usb-input-test:
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -fsanitize=address,undefined ../tests/native/usb_input.c -o ../out/host/usb-input-test
	../out/host/usb-input-test
usb-script-test:
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -fsanitize=address,undefined ../tests/native/usb_script.c ../tools/glaze-usb/script.c ../tools/glaze-usb/screen.c $(USB_FLAGS) -o ../out/host/usb-script-test
	../out/host/usb-script-test ../tests/ui/*.txt
.PHONY: usb-input-test usb-script-test
usb-cleanup-test:
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -fsanitize=address,undefined ../tests/native/usb_cleanup.c ../tools/glaze-usb/cleanup.c $(USB_FLAGS) -o ../out/host/usb-cleanup-test
	../out/host/usb-cleanup-test
.PHONY: usb-cleanup-test

# Real native CLI, with USB calls forwarded into the emulator by a test peer.
glaze-usb-emulator: $(USB_SOURCES) ../tests/native/usb_libusb_stub.c ../lib/include/glaze/debug_net.h $(O)/libwolfssl.a
	@mkdir -p ../out/host
	$(USB_CC) -std=gnu11 -O2 -g -Wall -Wextra -I../lib/include $(shell $(USB_PKG_CONFIG) --cflags libusb-1.0) $(USB_TLS) $(USB_SOURCES) ../tests/native/usb_libusb_stub.c $(O)/libwolfssl.a -o ../out/host/glaze-usb-emulator
.PHONY: glaze-usb-emulator

.PHONY: sheet
sheet: $(O)/sheet
$(O)/sheet: sheet.c hostres.c $(GFX)
	$(CC) $(CFLAGS) -o $@ $^

.PHONY: preview
preview: $(O)/preview
$(O)/preview: preview.c hostres.c $(GFX) $(SB)
	$(CC) $(CFLAGS) -o $@ $^

# preview and sheet write their BMPs into $(O); they run from host/ for their asset paths.
shots: $(O)/preview
	$(O)/preview
	for f in lock lock_drag unlock_anim home home_dusk_pressed home_badges home_banner lock_notes; do sips -s format png $(O)/$$f.bmp --out $(O)/$$f.png >/dev/null; done

.PHONY: fstest
fstest: $(O)/fstest
$(O)/fstest: fstest.c $(FS)
	$(CC) $(FS_CFLAGS) -o $@ $^

.PHONY: fattest
fattest: $(O)/fattest
$(O)/fattest: fattest.c $(FAT) ../servers/fs/card.h
	$(CC) $(FAT_CFLAGS) -o $@ fattest.c $(FAT)

# fattest on fresh FAT12, FAT16 and FAT32 images, each checked with fsck.fat
# afterwards (needs dosfstools and mtools). The FAT16 run also uses a 10 MiB
# FAT16 volume (mkfs.fat's FAT16 needs 9) as /NAND, in Windows CE's NAND
# layout (nanddisk.c).
fat-test: $(O)/fattest
	set -e; for f in "12 4" "16 16" "32 64"; do set -- $$f; \
	    ../tools/mkcfimg.sh $(O)/fat$$1.img $$2 $$1 ../userdata/Documents >/dev/null; \
	    nand=; [ $$1 = 16 ] && { ../tools/mkcfimg.sh $(O)/nand16.img 10 16 ../userdata/Documents >/dev/null; nand=$(O)/nand16.img; }; \
	    $(O)/fattest $(O)/fat$$1.img $$nand >$(O)/fat$$1.log || { grep FAIL $(O)/fat$$1.log; exit 1; }; \
	    fsck.fat -n $(O)/fat$$1.img >/dev/null; [ -z "$$nand" ] || fsck.fat -n $$nand >/dev/null; \
	    echo "FAT$$1: fattest and fsck.fat pass$${nand:+ (with /NAND)}"; \
	    rm -f $(O)/fat$$1.img $(O)/fat$$1.log $$nand; done
.PHONY: fat-test

# The top-level Makefile builds its own copy into out/host for the image.
.PHONY: mkfsimg
mkfsimg: $(O)/mkfsimg
$(O)/mkfsimg: mkfsimg.c $(FS)
	$(CC) $(FS_CFLAGS) -o $@ $^

# wolfSSL is built once, without sanitizers (its constant-time code trips UBSan's shift checks).
WOLFSSL_OBJS := $(patsubst ../third_party/wolfssl/%.c,$(O)/wolfssl.o/%.o,$(WOLFSSL))
$(O)/wolfssl.o/%.o: ../third_party/wolfssl/%.c ../apps/browser/tls/user_settings.h
	@mkdir -p $(dir $@)
	$(CC) $(WOLFSSL_CFLAGS) -c -o $@ $<
$(O)/libwolfssl.a: $(WOLFSSL_OBJS)
	rm -f $@
	ar rcs $@ $^

WEB_OBJS := $(patsubst ../apps/browser/%.c,$(O)/browser.o/%.o,$(WEB))
$(O)/browser.o/%.o: ../apps/browser/%.c
	@mkdir -p $(dir $@)
	$(CC) $(WEB_CFLAGS) -MMD -MP -c -o $@ $<
$(O)/browser.o/script.o: ../out/include/js_bootstrap.h
-include $(WEB_OBJS:.o=.d)

.PHONY: browse
browse: $(O)/browse
$(O)/browse: browse.c hostres.c $(GFX) $(WEB_OBJS) $(wildcard ../apps/browser/*.h ../apps/browser/js_platform/*) $(O)/libwolfssl.a $(O)/libjerry.a ../out/include/js_bootstrap.h
	$(CC) $(WEB_CFLAGS) -o $@ browse.c hostres.c $(GFX) $(WEB_OBJS) $(O)/libwolfssl.a $(O)/libjerry.a -lm

# Pages that show one thing real sites do, checked on what the engine lays out.
.PHONY: browser-compat-test
browser-compat-test: $(O)/browse
	python3 ../tests/browser_compat.py $(O)/browse

.PHONY: mediatest
mediatest: $(O)/mediatest
$(O)/mediatest: mediatest.c $(MEDIA)
	$(CC) $(MEDIA_CFLAGS) -o $@ $^ -lm

clean:
	rm -rf $(O)

# Byte decoder and shared text editing, with ASan/UBSan.
# The NAND ECC decoder ported from Windows CE's nanddisk.dll (RAM boot /NAND).
nand-ecc-test: ../tests/native/nand_ecc.c ../kernel/drivers/nand_ecc.c ../kernel/drivers/nand_ecc.h
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o $(O)/$@ $(filter %.c,$^)
	$(O)/$@
.PHONY: nand-ecc-test

# libc's memcmp and bit counts, renamed so the host's remain to compare with.
libc-bits-test: ../tests/native/libc_bits.c ../lib/libc/string.c ../lib/libc/bitops.c
	$(CC) -std=gnu11 -O2 -ffreestanding -fno-builtin -I../lib/include -I../lib/libc/include \
		-Dmemcmp=gz_memcmp -c -o $(O)/libc_bits_string.o ../lib/libc/string.c
	$(CC) -std=gnu11 -O2 -ffreestanding -fno-builtin -D__ctzsi2=gz_ctz -D__clzsi2=gz_clz \
		-c -o $(O)/libc_bits_bitops.o ../lib/libc/bitops.c
	objcopy --keep-global-symbol=gz_memcmp $(O)/libc_bits_string.o
	objcopy --keep-global-symbol=gz_ctz --keep-global-symbol=gz_clz $(O)/libc_bits_bitops.o
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o $(O)/$@ $< $(O)/libc_bits_string.o $(O)/libc_bits_bitops.o
	$(O)/$@
.PHONY: libc-bits-test

# libc's vsnprintf against the host's: printf.c renamed, with
# tests/native/libc_printf_sys.h for glaze/sys.h; the test supplies gz_log.
libc-printf-test: ../tests/native/libc_printf.c ../tests/native/libc_printf_sys.h ../lib/libc/printf.c
	@mkdir -p $(O)
	$(CC) -std=gnu11 -O2 -fno-builtin -Wno-ignored-attributes -fsanitize=address,undefined -I../lib/include -I../lib/libc/include \
		-include ../tests/native/libc_printf_sys.h \
		-Dvsnprintf=gz_vsnprintf -Dsnprintf=gz_snprintf -Dprintf=gz_printf -Dputs=gz_puts \
		-c -o $(O)/libc_printf_lib.o ../lib/libc/printf.c
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o $(O)/$@ $< $(O)/libc_printf_lib.o
	$(O)/$@
.PHONY: libc-printf-test

# libc's 64-bit division (lib/libc/divdi3.c) against the host's.
divdi3-test: ../tests/native/divdi3.c ../lib/libc/divdi3.c
	$(CC) $(CFLAGS) -O2 -fno-builtin -o $(O)/$@ $(filter %.c,$^)
	$(O)/$@
.PHONY: divdi3-test

audio-driver-test: ../tests/native/audio_driver.c ../kernel/drivers/audio.c ../kernel/hw.h
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -fno-strict-aliasing -fsanitize=address,undefined -o ../out/host/$@ ../tests/native/audio_driver.c
	../out/host/$@
.PHONY: audio-driver-test

buzzer-driver-test: ../tests/native/buzzer_driver.c ../kernel/drivers/buzzer.c ../kernel/hw.h
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o ../out/host/$@ ../tests/native/buzzer_driver.c
	../out/host/$@
.PHONY: buzzer-driver-test

battery-test: ../tests/native/battery_driver.c ../kernel/drivers/battery.c ../lib/include/glaze/battery.h ../kernel/hw.h
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o ../out/host/$@ ../tests/native/battery_driver.c
	../out/host/$@
.PHONY: battery-test

audio-volume-test: ../tests/native/audio_volume.c ../tests/native/audio_volume_sys.h ../servers/audio/sound.c ../servers/audio/buzzer.c ../lib/media/wav.c ../lib/media/resample.c
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -fsanitize=address,undefined -include ../tests/native/audio_volume_sys.h -o ../out/host/$@ ../tests/native/audio_volume.c ../servers/audio/buzzer.c ../lib/media/wav.c ../lib/media/resample.c
	../out/host/$@
.PHONY: audio-volume-test

stowaway-test: ../tests/native/stowaway.c ../kernel/drivers/stowaway.c ../lib/ui/input.c $(EDIT)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o $(O)/$@ $^
	$(O)/$@
.PHONY: stowaway-test

elf-test: ../tests/native/elf.c ../lib/include/glaze/elf.h
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o $(O)/$@ ../tests/native/elf.c
	$(O)/$@
.PHONY: elf-test

store-catalog-test: ../tests/native/store_catalog.c ../apps/appstore/catalog.c
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o ../out/host/store-catalog-test $^
	../out/host/store-catalog-test ../out/store/catalog.txt
	python3 ../tests/store_generator.py
.PHONY: store-catalog-test

store-package-test: ../tests/native/store_packages.c ../lib/libc/pkg.c
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o ../out/host/store-package-test $^
	../out/host/store-package-test
.PHONY: store-package-test

store-http-test: $(O)/libwolfssl.a
	@mkdir -p ../out/host
	$(CC) $(WEB_CFLAGS) -o ../out/host/store-http-test ../tests/native/store_http.c \
	    ../apps/browser/http.c ../apps/browser/conn.c ../apps/browser/tls.c ../apps/browser/url.c \
	    ../apps/appstore/inflate.c $(O)/libwolfssl.a
	python3 ../tests/store_transport.py
.PHONY: store-http-test

radio-test: ../tests/native/radio_protocol.c ../servers/audio/radio_protocol.c ../servers/audio/radio_mp3.c
	@mkdir -p ../out/host
	$(CC) $(MEDIA_CFLAGS) -o ../out/host/radio-test $^ $(wildcard ../third_party/helix-mp3/*.c ../third_party/helix-mp3/real/*.c)
	../out/host/radio-test ../userdata/Music/Demo.mp3
.PHONY: radio-test

radio-worker-test: $(O)/libwolfssl.a
	@mkdir -p ../out/host
	$(CC) $(MEDIA_CFLAGS) -I../apps/browser -I../third_party/wolfssl -I../apps/browser/tls \
	    -DWOLFSSL_USER_SETTINGS -include ../tests/native/radio_host.h -o ../out/host/radio-worker-test \
	    ../tests/native/radio_worker.c ../servers/audio/radio.c ../servers/audio/radio_protocol.c ../servers/audio/radio_mp3.c \
	    ../apps/browser/conn.c ../apps/browser/tls.c ../apps/browser/url.c \
	    $(wildcard ../third_party/helix-mp3/*.c ../third_party/helix-mp3/real/*.c) $(O)/libwolfssl.a -lpthread
	python3 ../tests/radio_transport.py
.PHONY: radio-worker-test

# Scalar pixel oracles for clipping, alpha, dithering and three-slice sampling.
gfx-test: ../tests/native/gfx.c ../lib/gfx/gfx.c ../lib/gfx/blit.c
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -O2 -fsanitize=address,undefined -o ../out/host/gfx-test $^
	../out/host/gfx-test
.PHONY: gfx-test

settings-test: ../tests/native/settings.c ../tests/native/settings_sys.h ../apps/settings/main.c ../lib/libc/touch.c ../lib/libc/hardicon.c
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -O2 -fsanitize=address,undefined -include ../tests/native/settings_sys.h \
	    -o ../out/host/settings-test ../tests/native/settings.c hostres.c $(GFX) \
	    ../lib/ui/widgets.c ../lib/ui/keypad.c ../lib/ui/transition.c ../lib/ui/text.c ../lib/libc/tz.c ../lib/libc/touch.c ../lib/libc/hardicon.c ../lib/libc/wifi.c ../lib/ui/input.c $(KEYBOARD)
	../out/host/settings-test
.PHONY: settings-test

reboot-test: ../tests/native/reboot.c ../kernel/reboot.c
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o ../out/host/reboot-test ../tests/native/reboot.c
	../out/host/reboot-test
.PHONY: reboot-test

entropy-test:
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -DGLAZE_ENTROPY_TEST -fsanitize=address,undefined \
	    -o ../out/host/entropy-test ../tests/native/entropy.c ../lib/libc/entropy.c
	../out/host/entropy-test
	$(CC) $(CFLAGS) -O0 -DGLAZE_ENTROPY_TEST -I../third_party/jitterentropy -fsanitize=address,undefined \
	    -o ../out/host/entropy-clock-test ../tests/native/entropy_clock.c ../lib/libc/entropy_jent.c
	../out/host/entropy-clock-test
.PHONY: entropy-test

hardicon-test: ../tests/native/hardicon.c ../lib/libc/hardicon.c ../servers/springboard/hardicon.c
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -O2 -fsanitize=address,undefined -o ../out/host/hardicon-test $^
	../out/host/hardicon-test
.PHONY: hardicon-test

layout-test: ../tests/native/layout.c ../servers/springboard/layout.c
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -O2 -fsanitize=address,undefined -o ../out/host/layout-test $^
	../out/host/layout-test
.PHONY: layout-test

touch-calibration-test: ../tests/native/touch_calibration.c ../lib/libc/touch.c ../servers/springboard/calibration.c
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -O2 -fsanitize=address,undefined -o ../out/host/touch-calibration-test $^
	../out/host/touch-calibration-test
.PHONY: touch-calibration-test

display-test: ../tests/native/display.c ../tests/native/display_sys.h ../kernel/drivers/display.c ../servers/springboard/display.c
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -O2 -fsanitize=address,undefined -include ../tests/native/display_sys.h \
	    -o ../out/host/display-test ../tests/native/display.c ../servers/springboard/display.c
	../out/host/display-test
.PHONY: display-test

idle-test: ../tests/native/idle.c ../servers/springboard/idle.c ../servers/springboard/idle.h ../lib/include/glaze/display.h
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -O2 -fsanitize=address,undefined -o ../out/host/idle-test ../tests/native/idle.c ../servers/springboard/idle.c
	../out/host/idle-test
.PHONY: idle-test

taskmanager-test: ../tests/native/taskmanager.c ../tests/native/taskmanager_sys.h ../packages/taskmanager/main.c ../packages/taskmanager/monitor.c
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -O2 -fsanitize=address,undefined -include ../tests/native/taskmanager_sys.h \
	    -o ../out/host/taskmanager-test ../tests/native/taskmanager.c ../packages/taskmanager/monitor.c \
	    hostres.c $(GFX) ../lib/ui/widgets.c ../lib/ui/list.c
	../out/host/taskmanager-test
.PHONY: taskmanager-test

ui-events-test: ../tests/native/ui_events.c ../tests/native/ui_events_sys.h ../lib/ui/app.c ../lib/ui/list.c $(KEYBOARD)
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -O2 -fsanitize=address,undefined -include ../tests/native/ui_events_sys.h \
	    -o ../out/host/ui-events-test ../tests/native/ui_events.c ../lib/ui/app.c ../lib/ui/list.c \
	    $(KEYBOARD) ../lib/ui/widgets.c hostres.c $(GFX)
	../out/host/ui-events-test
.PHONY: ui-events-test

handwriting-test: ../tests/native/handwriting.c $(KEYBOARD) ../lib/ui/input.c
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -O2 -fsanitize=address,undefined -include ../tests/native/ui_events_sys.h -o ../out/host/handwriting-test $^ ../lib/ui/widgets.c hostres.c $(GFX)
	../out/host/handwriting-test
.PHONY: handwriting-test

gfx-bench: ../tests/native/gfx_bench.c ../lib/gfx/gfx.c ../lib/gfx/blit.c ../lib/gfx/text.c
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -O2 -o ../out/host/gfx-bench $^
	../out/host/gfx-bench
.PHONY: gfx-bench

gfx-text-test: ../tests/native/gfx_text.c ../lib/gfx/gfx.c ../lib/gfx/text.c
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -O2 -fsanitize=address,undefined -o ../out/host/gfx-text-test $^
	../out/host/gfx-text-test ../assets/gen/*.gzf
.PHONY: gfx-text-test

ftl-bch-test: ../tests/native/ftl_bch.c $(FS)
	@mkdir -p ../out/host
	$(CC) $(FS_CFLAGS) -DFTL_SELFTEST -fsanitize=address,undefined -o ../out/host/ftl-bch-test $^
	../out/host/ftl-bch-test
.PHONY: ftl-bch-test

ftl-verify-test: ../tests/native/ftl_verify.c $(FS)
	@mkdir -p ../out/host
	$(CC) $(FS_CFLAGS) -fsanitize=address,undefined -o ../out/host/ftl-verify-test $^
	../out/host/ftl-verify-test
.PHONY: ftl-verify-test

ftl-verify-bench: ../tests/native/ftl_verify.c $(FS)
	@mkdir -p ../out/host
	$(CC) $(FS_CFLAGS) -DFTL_BENCH -o ../out/host/ftl-verify-bench $^
	../out/host/ftl-verify-bench
.PHONY: ftl-verify-bench

wince-test: ../tests/native/wince_pe.c ../apps/wince/pe.c ../apps/wince/pe.h ../tests/native/wince_datetime.c ../apps/wince/datetime.c ../apps/wince/datetime.h ../tests/native/wince_ordinals.c ../apps/wince/ordinals.c ../apps/wince/ordinals.h
wince-test: ../tests/native/wince_registry.c ../apps/wince/registry.c ../apps/wince/registry.h ../tests/native/wince_dialog.c ../apps/wince/dialog_template.c ../apps/wince/dialog_template.h
wince-test: ../tests/native/wince_property.c ../apps/wince/property_state.c ../apps/wince/property_state.h ../apps/wince/number.c ../apps/wince/number.h
wince-test: ../tests/native/wince_text.c ../apps/wince/text.c ../apps/wince/text.h
wince-test: ../tests/native/wince_runtime.c ../tests/native/wince_runtime_host.h ../tests/native/wince_filemeta.c ../apps/wince/filemeta.c ../apps/wince/thread.c ../apps/wince/mapping.c ../apps/wince/timezone.c ../apps/wince/scan.c ../apps/wince/string.c ../apps/wince/view.c ../apps/wince/winsock.c ../apps/wince/ole.c
wince-test: ../apps/wince/list.c ../apps/wince/date_control.c ../apps/wince/tabs.c
wince-test: ../apps/wince/database.c ../apps/wince/dbstore.c ../apps/wince/dbstore.h ../tests/native/wince_database.c
	@mkdir -p ../out/host
	$(CC) -std=c11 -g -Wall -Wextra -I../lib/include -fsanitize=address,undefined \
	    -o ../out/host/wince-pe-test ../tests/native/wince_pe.c ../apps/wince/pe.c
	../out/host/wince-pe-test
	$(CC) -std=c11 -g -Wall -Wextra -I../lib/include -fsanitize=address,undefined \
	    -o ../out/host/wince-datetime-test ../tests/native/wince_datetime.c ../apps/wince/datetime.c
	../out/host/wince-datetime-test
	$(CC) -std=c11 -g -Wall -Wextra -I../lib/include -fsanitize=address,undefined \
	    -o ../out/host/wince-ordinals-test ../tests/native/wince_ordinals.c ../apps/wince/ordinals.c
	../out/host/wince-ordinals-test
	$(CC) -std=c11 -g -Wall -Wextra -I../lib/include -fsanitize=address,undefined \
	    -o ../out/host/wince-registry-test ../tests/native/wince_registry.c ../apps/wince/registry.c ../apps/wince/pe.c
	../out/host/wince-registry-test
	$(CC) -std=c11 -g -Wall -Wextra -I../lib/include -fsanitize=address,undefined \
	    -o ../out/host/wince-dialog-test ../tests/native/wince_dialog.c ../apps/wince/dialog_template.c ../apps/wince/pe.c
	../out/host/wince-dialog-test
	$(CC) -std=c11 -g -Wall -Wextra -I../lib/include -fsanitize=address,undefined \
	    -o ../out/host/wince-property-test ../tests/native/wince_property.c ../apps/wince/property_state.c ../apps/wince/number.c
	../out/host/wince-property-test
	$(CC) -std=c11 -g -Wall -Wextra -I../lib/include -fsanitize=address,undefined \
	    -o ../out/host/wince-text-test ../tests/native/wince_text.c ../apps/wince/text.c
	../out/host/wince-text-test
	$(CC) -std=c11 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -I../lib/include \
	    -fsanitize=address,undefined -pthread -o ../out/host/wince-runtime-test ../tests/native/wince_runtime.c ../apps/wince/pe.c ../apps/wince/datetime.c ../lib/libc/tz.c
	../out/host/wince-runtime-test
	$(CC) -std=c11 -g -Wall -Wextra -I../lib/include -fsanitize=address,undefined -D_DEFAULT_SOURCE \
	    -o ../out/host/wince-filemeta-test ../tests/native/wince_filemeta.c ../apps/wince/datetime.c ../apps/wince/pe.c
	../out/host/wince-filemeta-test
	$(CC) -std=c11 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -I../lib/include -fsanitize=address,undefined \
	    -o ../out/host/wince-database-test ../tests/native/wince_database.c ../apps/wince/pe.c ../apps/wince/dbstore.c
	../out/host/wince-database-test
	python3 ../tests/wince_assets_test.py
	python3 ../tests/wince_runner_test.py
.PHONY: wince-test

# GlazeBoot's hand-over plan (wince/glazeboot/plan.h) on a RAM model.
glazeboot-test: ../tests/native/glazeboot_plan.c ../wince/glazeboot/plan.h
	@mkdir -p ../out/host
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -fsanitize=address,undefined \
	    -o ../out/host/glazeboot-plan-test ../tests/native/glazeboot_plan.c
	../out/host/glazeboot-plan-test
.PHONY: glazeboot-test

../out/host/wince-fs-read: ../tests/native/wince_fs_read.c $(FS)
	@mkdir -p ../out/host
	$(CC) $(FS_CFLAGS) -o $@ $^

# Keep the engine private to Browser. Native tests use the same feature profile.
$(O)/jerry.o: ../third_party/jerryscript/jerryscript.c ../third_party/jerryscript/jerryscript.h ../apps/browser/js_config.h
	$(CC) -std=c99 -O2 -g -I../third_party/jerryscript -include ../apps/browser/js_config.h -c -o $@ $<
$(O)/libjerry.a: $(O)/jerry.o
	ar rcs $@ $^
../out/include/js_bootstrap.h: ../apps/browser/js_dom.js ../apps/browser/js_web.js ../tools/mkjsbootstrap.py
	python3 ../tools/mkjsbootstrap.py $< $@

../out/host/browser-platform.o: browse.c
	@mkdir -p ../out/host
	$(CC) $(WEB_CFLAGS) -Dmain=browser_preview_main -c -o $@ $<
browser-test: $(WEB_OBJS) $(O)/libwolfssl.a $(O)/libjerry.a ../out/host/browser-platform.o
	$(CC) $(WEB_CFLAGS) -o ../out/host/browser-test ../tests/native/browser_script.c \
	    ../out/host/browser-platform.o hostres.c $(GFX) $(WEB_OBJS) $(O)/libwolfssl.a $(O)/libjerry.a -lm
	python3 ../tests/browser_transport.py ../out/host/browser-test
.PHONY: browser-test

# Opt-in live Google Search workflow; build only, never run as part of CI.
browser-google: $(WEB_OBJS) $(O)/libwolfssl.a $(O)/libjerry.a ../out/host/browser-platform.o
	$(CC) $(WEB_CFLAGS) -o ../out/host/browser-google ../tests/native/browser_google.c \
	    ../out/host/browser-platform.o hostres.c $(GFX) $(WEB_OBJS) $(O)/libwolfssl.a $(O)/libjerry.a -lm
.PHONY: browser-google

.PHONY: net-driver-test
net-driver-test: ../tests/native/net_driver.c ../kernel/drivers/net.c
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -falign-functions=16 -fsanitize=address,undefined -o ../out/host/net-driver-test ../tests/native/net_driver.c
	../out/host/net-driver-test

# Optional peer-model tests; point at the matching be300 worktree.
BE300_SOURCE ?= ../../be300
.PHONY: wl54cf-driver-test
wl54cf-driver-test: ../tests/native/wl54cf_driver.c ../kernel/drivers/wl54cf.c ../kernel/drivers/wl54cf_io.h
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -fsanitize=address,undefined -I$(BE300_SOURCE)/src -Dwl54cf_init=wl54cf_model_init -c $(BE300_SOURCE)/src/hw/wl54cf.c -o ../out/host/wl54cf-model.o
	$(CC) $(CFLAGS) -fsanitize=address,undefined -I$(BE300_SOURCE)/src ../tests/native/wl54cf_driver.c ../out/host/wl54cf-model.o $(BE300_SOURCE)/src/hw/wifi_security.c -o ../out/host/wl54cf-driver-test
	../out/host/wl54cf-driver-test

OPENSSL_FLAGS ?= -I/opt/homebrew/opt/openssl@3/include
OPENSSL_LIBS ?= -L/opt/homebrew/opt/openssl@3/lib -lcrypto
HOSTAP := $(shell find ../third_party/hostap/src -name '*.c')
HOSTAP_FLAGS := $(CFLAGS) -O2 -include ../servers/net/wpa/config.h -I../third_party/hostap/src -I../third_party/hostap/src/utils -I../servers/net
.PHONY: wifi-supplicant-test xi825-test
wifi-supplicant-test:
	@mkdir -p ../out/host
	$(CC) $(HOSTAP_FLAGS) -DBE300_WIFI_OPENSSL -I$(BE300_SOURCE)/src $(OPENSSL_FLAGS) -fsanitize=address,undefined -o ../out/host/wifi-supplicant-test ../tests/native/wifi_supplicant.c ../servers/net/wifi_supplicant.c ../lib/libc/wifi.c $(HOSTAP) $(BE300_SOURCE)/src/hw/wifi_security.c $(OPENSSL_LIBS)
	../out/host/wifi-supplicant-test
xi825-test:
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -O2 -fsanitize=address,undefined -Dxi825_init=prism_init -c $(BE300_SOURCE)/src/hw/xi825.c -o ../out/host/xi825-model.o
	$(CC) $(CFLAGS) -O2 -fsanitize=address,undefined -I$(BE300_SOURCE)/src/hw -o ../out/host/xi825-test ../tests/native/xi825.c ../out/host/xi825-model.o $(BE300_SOURCE)/src/hw/wifi_security.c
	../out/host/xi825-test

.PHONY: wifi-mkfs
wifi-mkfs:
	@mkdir -p ../out/host
	$(CC) $(FS_CFLAGS) -o ../out/host/wifi-mkfs ../host/mkfsimg.c $(FS)

store-worker-test: $(O)/libwolfssl.a
	@mkdir -p ../out/host
	$(CC) $(WEB_CFLAGS) -I../apps/browser -include ../tests/native/radio_host.h -o ../out/host/store-worker-test \
	    ../tests/native/store_worker.c ../apps/appstore/worker.c ../apps/appstore/schedule.c \
	    ../apps/appstore/catalog.c ../lib/libc/pkg.c ../lib/gfx/gfx.c ../lib/gfx/blit.c \
	    $(O)/libwolfssl.a -lpthread
	../out/host/store-worker-test
.PHONY: store-worker-test

.PHONY: module-test
module-test: ../tests/native/module.c ../kernel/module.c
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o ../out/host/module-test ../tests/native/module.c
	../out/host/module-test ../out/drivers/ne2000.ko ../out/drivers/xi825.ko ../out/drivers/wl54cf.ko ../out/drivers/cfuart.ko ../out/drivers/3c589.ko ../out/drivers/hermes.ko

.PHONY: service-test
service-test: ../tests/native/service.c ../kernel/service.c
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o ../out/host/service-test ../tests/native/service.c
	../out/host/service-test

../out/host/debug-net-fixture: ../tests/native/debug_net_native.c ../servers/usb/network.c ../servers/usb/network.h ../lib/include/glaze/debug_net.h $(O)/libwolfssl.a
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) $(USB_TLS) -fsanitize=address,undefined $(filter %.c,$^) $(O)/libwolfssl.a -o $@
.PHONY: debug-net-test
debug-net-test: ../out/host/debug-net-fixture ../out/host/glaze-usb
	python3 ../tests/debug_network.py
../out/host/debug-net-client: ../tests/native/debug_net_client.c ../tools/glaze-usb/network.c ../tools/glaze-usb/network.h ../lib/include/glaze/debug_net.h $(O)/libwolfssl.a
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) $(USB_TLS) $(shell $(USB_PKG_CONFIG) --cflags libusb-1.0) $(filter %.c,$^) $(O)/libwolfssl.a -o $@
debug-net-test: ../out/host/debug-net-client

wince-nand-test: ../tests/native/wince_assets_native.c ../tests/wince_nand_assets.py ../apps/wince/assets.c ../apps/wince/assets.h ../apps/wince/pe.c ../apps/wince/exeicon.c ../apps/wince/exeicon.h
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o ../out/host/wince-assets-native ../tests/native/wince_assets_native.c ../apps/wince/assets.c ../apps/wince/pe.c ../apps/wince/exeicon.c
	python3 ../tests/wince_nand_assets.py
.PHONY: wince-nand-test

# OTA updates (docs/os/OTA.md): the kernel's verification and journal and the
# SPL's apply engine over the read-only flash stack, on a NAND model with
# power cuts, a failing block and a damaged staged file (tests/ota_test.py).
OTA_TP := ../third_party/puff/puff.c ../third_party/littlefs/lfs.c ../third_party/littlefs/lfs_util.c \
    ../third_party/dhara/dhara/map.c ../third_party/dhara/dhara/journal.c ../third_party/dhara/dhara/error.c \
    ../third_party/dhara/ecc/bch.c ../third_party/dhara/ecc/gf13.c \
    $(addprefix ../third_party/wolfssl/wolfcrypt/src/,ed25519.c ge_low_mem.c fe_low_mem.c sha512.c sha256.c)
OTA_DEFS := -I../third_party/littlefs -I../third_party/dhara -I../third_party/puff -I../third_party/wolfssl -I../kernel/crypto \
    -I../kernel -DWOLFSSL_USER_SETTINGS -DFTL_READONLY -DLFS_READONLY -DLFS_NO_MALLOC -DLFS_NO_DEBUG \
    -DLFS_NO_WARN -DLFS_NO_ERROR -DLFS_NO_ASSERT
OTA_TP_OBJS := $(patsubst ../third_party/%.c,../out/host/ota.o/%.o,$(OTA_TP))
$(OTA_TP_OBJS): ../out/host/ota.o/%.o: ../third_party/%.c
	@mkdir -p $(dir $@)
	$(CC) -std=gnu11 -O2 -g -w $(OTA_DEFS) -c -o $@ $<
../out/host/ota-test: ../tests/native/ota.c ../boot/ota_apply.c ../boot/ota_apply.h ../kernel/ota_stage.c ../kernel/ota_stage.h \
    ../kernel/drivers/nand_boot.c ../kernel/drivers/nand_ecc.c ../servers/fs/ftl.c ../lib/include/glaze/ota.h $(OTA_TP_OBJS)
	$(CC) $(CFLAGS) -O2 -fsanitize=address,undefined $(OTA_DEFS) -o $@ ../tests/native/ota.c ../boot/ota_apply.c \
	    ../kernel/ota_stage.c ../kernel/drivers/nand_boot.c ../kernel/drivers/nand_ecc.c ../servers/fs/ftl.c $(OTA_TP_OBJS)
ota-test: ../out/host/ota-test $(O)/mkfsimg
	python3 ../tests/ota_test.py $(O)/mkfsimg ../out/host/ota-test
.PHONY: ota-test

# Hardware packages (glaze/hwpkg.h): kernel/hwfile.c's registry, fetch and
# Ed25519/SHA-256 checks over packages tools/mkhwpkg.py signs with a test key.
# Natively (OpenSSL 3), like ota-test.
HWFILE_CRYPTO := $(filter %ed25519.o %ge_low_mem.o %fe_low_mem.o %sha512.o %sha256.o,$(OTA_TP_OBJS))
../out/host/hwfile-test: ../tests/native/hwfile.c ../kernel/hwfile.c ../kernel/hwfile.h ../lib/include/glaze/hwpkg.h $(HWFILE_CRYPTO)
	$(CC) $(CFLAGS) -O1 -fsanitize=address,undefined $(OTA_DEFS) -DHWFILE_TEST -DGLAZE_VERSION='"9.0"' -o $@ \
	    ../tests/native/hwfile.c ../kernel/hwfile.c $(HWFILE_CRYPTO)
hwfile-test: ../out/host/hwfile-test
	python3 ../tests/hwfile_test.py ../out/host/hwfile-test
.PHONY: hwfile-test

# Production early boot I/O with fake registers and failure injection.
boot-init-test: ../tests/native/spl_io.c ../boot/spl.c ../kernel/drivers/nand.c ../tests/native/timer_boot.c ../kernel/timer.c ../tests/native/irq_boot.c ../kernel/drivers/irq.c ../tests/native/rtc_boot.c ../kernel/drivers/rtc.c
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -fsanitize=address,undefined ../tests/native/spl_io.c ../kernel/drivers/nand_boot.c ../kernel/drivers/nand_ecc.c -o ../out/host/spl-io-test
	../out/host/spl-io-test
	$(CC) $(CFLAGS) -fsanitize=address,undefined ../tests/native/timer_boot.c -o ../out/host/timer-boot-test
	../out/host/timer-boot-test
	$(CC) $(CFLAGS) -fsanitize=address,undefined ../tests/native/irq_boot.c -o ../out/host/irq-boot-test
	../out/host/irq-boot-test
	$(CC) $(CFLAGS) -fsanitize=address,undefined -I../kernel ../tests/native/rtc_boot.c -o ../out/host/rtc-boot-test
	../out/host/rtc-boot-test
.PHONY: boot-init-test

nand-boot-test: ../tests/native/nand_boot.c ../tests/native/nand_ecc.c ../kernel/drivers/nand_boot.c ../kernel/drivers/nand_boot.h ../kernel/drivers/nand_ecc.c
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -fsanitize=address,undefined ../tests/native/nand_boot.c ../kernel/drivers/nand_boot.c ../kernel/drivers/nand_ecc.c -o ../out/host/nand-boot-test
	../out/host/nand-boot-test
.PHONY: nand-boot-test

../out/host/nand-inventory: nand_inventory.c ../kernel/drivers/nand_boot.c ../kernel/drivers/nand_ecc.c ../kernel/drivers/nand_boot.h ../kernel/drivers/nand_ecc.h
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -O2 nand_inventory.c ../kernel/drivers/nand_boot.c ../kernel/drivers/nand_ecc.c -o $@
nand-image-tools: ../out/host/nand-inventory mkfsimg
.PHONY: nand-image-tools

ftl-factory-test: ../tests/native/ftl_factory_bad.c $(FS)
	@mkdir -p ../out/host
	$(CC) $(FS_CFLAGS) -fsanitize=address,undefined -o ../out/host/ftl-factory-test $^
	../out/host/ftl-factory-test
.PHONY: ftl-factory-test

nand-backup-test: ../tests/native/nand_backup.c ../tools/glaze-usb/nand.c
	@mkdir -p ../out/host
	$(USB_CC) $(CFLAGS) $(USB_FLAGS) -fsanitize=address,undefined $^ -o ../out/host/nand-backup-test
	../out/host/nand-backup-test
.PHONY: nand-backup-test

usb-client-test:
	@mkdir -p ../out/host
	$(USB_CC) $(CFLAGS) $(USB_FLAGS) ../tests/native/usb_client_failures.c ../tools/glaze-usb/pcconnect.c -o ../out/host/usb-client-test
	../out/host/usb-client-test
.PHONY: usb-client-test

nand-policy-test: ../tests/native/nand_raw_policy.c ../kernel/drivers/flash.c ../kernel/drivers/nand_ecc.c
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -fsanitize=address,undefined ../tests/native/nand_raw_policy.c ../kernel/drivers/nand_ecc.c -o ../out/host/nand-policy-test
	../out/host/nand-policy-test
.PHONY: nand-policy-test

cf-boot-test: ../tests/native/cf_boot.c ../kernel/drivers/cf.c
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -fsanitize=address,undefined ../tests/native/cf_boot.c -o ../out/host/cf-boot-test
	../out/host/cf-boot-test
.PHONY: cf-boot-test

veo-test: ../tests/native/veo.c ../kernel/drivers/veo.c ../lib/media/veo_jpeg.c ../lib/media/jpeg.c ../lib/include/glaze/veo.h
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -fsanitize=address,undefined ../tests/native/veo.c ../lib/media/veo_jpeg.c ../lib/media/jpeg.c -o ../out/host/veo-test
	../out/host/veo-test
.PHONY: veo-test

nand-program-test: ../tests/native/nand_program.c ../kernel/drivers/nand.c $(BE300_SOURCE)/src/hw/nand.c $(BE300_SOURCE)/src/hw/nand_restore.c
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -I$(BE300_SOURCE)/src -fsanitize=address,undefined ../tests/native/nand_program.c $(BE300_SOURCE)/src/hw/nand.c $(BE300_SOURCE)/src/hw/nand_restore.c -o ../out/host/nand-program-test
	../out/host/nand-program-test
.PHONY: nand-program-test

../out/host/mkcf-recovery: mkcf_recovery.c $(FAT) ../third_party/fatfs/ffconf.h
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -I../servers/fs -I../third_party/fatfs -fsanitize=address,undefined mkcf_recovery.c $(FAT) -o $@
cf-recovery-tools: ../out/host/mkcf-recovery
.PHONY: cf-recovery-tools

cf-read-test: ../tests/native/cf_read.c ../tools/glaze-usb/cf.c
	@mkdir -p ../out/host
	$(USB_CC) $(CFLAGS) $(USB_FLAGS) -fsanitize=address,undefined $^ -o ../out/host/cf-read-test
	../out/host/cf-read-test
.PHONY: cf-read-test

nand-writer-test: ../tests/native/nand_writer.c ../tools/glaze-usb/nand_write.c ../kernel/nand_repair.c $(O)/libwolfssl.a
	@mkdir -p ../out/host
	$(USB_CC) $(CFLAGS) $(USB_FLAGS) $(USB_TLS) -fsanitize=address,undefined ../tests/native/nand_writer.c ../tools/glaze-usb/nand_write.c $(O)/libwolfssl.a -o ../out/host/nand-writer-test
	../out/host/nand-writer-test
.PHONY: nand-writer-test

usb-ce-staging-test: ../tests/native/usb_ce_staging.c ../tools/glaze-usb/client.c ../tools/glaze-usb/pcconnect.h
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -fsanitize=address,undefined ../tests/native/usb_ce_staging.c $(USB_FLAGS) -o ../out/host/usb-ce-staging-test
	../out/host/usb-ce-staging-test
.PHONY: usb-ce-staging-test

installer-ui-test: ../tests/native/installer_ui.c ../servers/springboard/installer.c ../servers/springboard/installer.h hostres.c $(GFX)
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -O2 -o ../out/host/installer-ui-test ../tests/native/installer_ui.c ../servers/springboard/installer.c hostres.c $(GFX)
	../out/host/installer-ui-test
.PHONY: installer-ui-test


bluetooth-test:
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -fsanitize=address,undefined ../tests/native/bluetooth.c ../servers/bluetooth/bcsp.c ../servers/bluetooth/controller.c ../servers/bluetooth/serial.c -o ../out/host/bluetooth-test
	../out/host/bluetooth-test
.PHONY: bluetooth-test

bluetooth-driver-test:
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -fsanitize=address,undefined ../tests/native/bluetooth_driver.c -o ../out/host/bluetooth-driver-test
	../out/host/bluetooth-driver-test
.PHONY: bluetooth-driver-test

bt-echo:
	@mkdir -p ../out/host/GlazeBluetoothEcho.app/Contents/MacOS
	clang -fobjc-arc -O2 -Wall -Wextra -framework Foundation -framework IOBluetooth ../tools/bt_echo/main.m -o ../out/host/GlazeBluetoothEcho.app/Contents/MacOS/GlazeBluetoothEcho
	cp ../tools/bt_echo/Info.plist ../out/host/GlazeBluetoothEcho.app/Contents/Info.plist
	codesign --force --sign - ../out/host/GlazeBluetoothEcho.app
.PHONY: bt-echo

bluetooth-serial-test:
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -fsanitize=address,undefined ../tests/native/bluetooth_serial.c -o ../out/host/bluetooth-serial-test
	../out/host/bluetooth-serial-test
.PHONY: bluetooth-serial-test

modem-chat-test:
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -fsanitize=address,undefined ../tests/native/modem_chat.c ../servers/net/hayes.c ../lib/libc/net_dial.c -o ../out/host/modem-chat-test
	../out/host/modem-chat-test
.PHONY: modem-chat-test

# kernel/romfs.c and kernel/lz4.c on archives from tools/mkromfs.py, plain and --lz4.
romfs-lz4-test: ../tests/native/romfs_lz4.c ../kernel/romfs.c ../kernel/lz4.c ../tools/mkromfs.py \
    ../tools/romfs.py ../tools/lz4block.py ../tests/romfs_lz4_data.py
	python3 -I ../tests/romfs_lz4_data.py ../out/host/romfs-lz4
	$(CC) $(CFLAGS) -fsanitize=address,undefined ../tests/native/romfs_lz4.c -o ../out/host/romfs-lz4-test
	../out/host/romfs-lz4-test ../out/host/romfs-lz4/plain.img ../out/host/romfs-lz4/lz4.img ../out/host/romfs-lz4/f
.PHONY: romfs-lz4-test

gps-test: ../tests/native/gps.c ../kernel/gps.c ../lib/gps/gps.c ../lib/gps/gps_tables.h ../lib/include/glaze/gps.h
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -fsanitize=address,undefined ../tests/native/gps.c -lm -o ../out/host/gps-test
	../out/host/gps-test
.PHONY: gps-test

modem-driver-test:
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -fsanitize=address,undefined ../tests/native/modem_driver.c -o ../out/host/modem-driver-test
	../out/host/modem-driver-test
.PHONY: modem-driver-test

bluetooth-client-test:
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -fsanitize=address,undefined ../tests/native/bluetooth_client.c -o ../out/host/bluetooth-client-test
	../out/host/bluetooth-client-test
.PHONY: bluetooth-client-test

connectivity-test:
	@mkdir -p ../out/host
	$(CC) $(CFLAGS) -fsanitize=address,undefined ../tests/native/connectivity.c hostres.c $(GFX) $(SB) -o ../out/host/connectivity-test
	../out/host/connectivity-test
.PHONY: connectivity-test

SBC := $(wildcard ../third_party/sbc/srce/*.c)
bluetooth-audio-test:
	$(CC) $(CFLAGS) -I../third_party/sbc/include -fsanitize=address,undefined -fno-sanitize=shift \
	    ../tests/native/bluetooth_audio.c ../servers/bluetooth/serial.c ../servers/bluetooth/source_sdp.c $(SBC) -o ../out/host/bluetooth-audio-test
	../out/host/bluetooth-audio-test ../out/host/bluetooth-tone.sbc
.PHONY: bluetooth-audio-test

audio-output-test:
	$(CC) $(CFLAGS) -include ../tests/native/audio_output_sys.h -fsanitize=address,undefined ../tests/native/audio_output.c -o ../out/host/audio-output-test
	../out/host/audio-output-test
.PHONY: audio-output-test

bluetooth-source-sdp-test:
	$(CC) $(CFLAGS) -fsanitize=address,undefined ../tests/native/bluetooth_source_sdp.c -o ../out/host/bluetooth-source-sdp-test
	../out/host/bluetooth-source-sdp-test
.PHONY: bluetooth-source-sdp-test

bluetooth-uart-test:
	$(CC) $(CFLAGS) -fsanitize=address,undefined ../tests/native/bluetooth_uart.c -o ../out/host/bluetooth-uart-test
	../out/host/bluetooth-uart-test
.PHONY: bluetooth-uart-test

wifi-retry-test: ../tests/native/wifi_retry.c ../servers/net/wifi_retry.h
	$(CC) $(CFLAGS) -fsanitize=address,undefined ../tests/native/wifi_retry.c -o ../out/host/wifi-retry-test
	../out/host/wifi-retry-test
.PHONY: wifi-retry-test

presenter-test:
	$(CC) $(CFLAGS) -fsanitize=address,undefined ../tests/native/presenter.c -o $(O)/presenter-test
	$(O)/presenter-test
.PHONY: presenter-test
presenter-lifecycle-test:
	$(CC) $(CFLAGS) -fsanitize=address,undefined ../tests/native/presenter_lifecycle.c -o $(O)/presenter-lifecycle-test
	$(O)/presenter-lifecycle-test
.PHONY: presenter-lifecycle-test
