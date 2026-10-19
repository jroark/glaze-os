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

# The browser engine, rendering pages to BMPs.
WEB := $(filter-out ../apps/browser/main.c,$(wildcard ../apps/browser/*.c))
WEB_CFLAGS := $(CFLAGS) -I../third_party/stb -fsanitize=address,undefined

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

browse: browse.c hostres.c $(GFX) $(WEB) ../apps/browser/web.h
	$(CC) $(WEB_CFLAGS) -o $@ browse.c hostres.c $(GFX) $(WEB)

mediatest: mediatest.c $(MEDIA)
	$(CC) $(MEDIA_CFLAGS) -o $@ $^ -lm

clean:
	rm -f sheet preview fstest fattest mkfsimg browse mediatest stowaway-test elf-test *.bmp *.png

# Byte decoder and shared text editing, with ASan/UBSan.
stowaway-test: ../tests/stowaway.c ../kernel/drivers/stowaway.c ../lib/ui/input.c
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o $@ $^
	./$@
.PHONY: stowaway-test

elf-test: ../tests/elf.c ../lib/include/glaze/elf.h
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o $@ ../tests/elf.c
	./$@
.PHONY: elf-test
