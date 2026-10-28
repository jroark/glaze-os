# Native host build. Deliberately use only Glaze protocol headers, not its libc.
CC ?= cc
PKG_CONFIG ?= pkg-config
SOURCES := $(wildcard glaze_usb/*.c)
.PHONY: all
all: ../bin/glaze-usb
../bin/glaze-usb: $(SOURCES) $(wildcard glaze_usb/*.h include/glaze/*.h)
	$(CC) -std=gnu11 -O2 -g -Wall -Wextra -Iinclude $(SOURCES) \
	    $(shell $(PKG_CONFIG) --cflags --libs libusb-1.0) -o $@
