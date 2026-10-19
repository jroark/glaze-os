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

# The browser engine, rendering pages to BMPs.
WEB := $(filter-out ../apps/browser/main.c,$(wildcard ../apps/browser/*.c))
WEB_CFLAGS := $(CFLAGS) -I../third_party/stb -fsanitize=address,undefined

all: sheet preview fstest mkfsimg browse

sheet: sheet.c hostres.c $(GFX)
	$(CC) $(CFLAGS) -o $@ $^

preview: preview.c hostres.c $(GFX) $(SB)
	$(CC) $(CFLAGS) -o $@ $^

shots: preview
	./preview
	for f in lock lock_drag unlock_anim home home_dusk_pressed; do sips -s format png $$f.bmp --out $$f.png >/dev/null; done

fstest: fstest.c $(FS)
	$(CC) $(FS_CFLAGS) -o $@ $^

# The top-level Makefile builds its own copy into out/host for the image.
mkfsimg: mkfsimg.c $(FS)
	$(CC) $(FS_CFLAGS) -o $@ $^

browse: browse.c hostres.c $(GFX) $(WEB) ../apps/browser/web.h
	$(CC) $(WEB_CFLAGS) -o $@ browse.c hostres.c $(GFX) $(WEB)

clean:
	rm -f sheet preview fstest mkfsimg browse *.bmp *.png
