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
# Native compiler for build tools ($(OUT)/buildtools/mkfsimg; out/host is host/Makefile's).
HOSTCC  ?= cc

OUT     ?= out
DYNAMIC ?= 1

# First user process started by the kernel.
INIT    ?= bin/springboard
BUILD_ID ?= $(shell git describe --always --dirty 2>/dev/null || echo dev)
# The OS version OTA updates compare (docs/os/OTA.md): dotted numeric, in VERSION.
GLAZE_VERSION := $(shell cat VERSION)
# Test builds: replay this input script (see kernel/drivers/input.c).
INPUT_SCRIPT ?=
# Read-only physical NAND diagnostic after a CE RAM handoff.
NAND_BOOT_PROBE ?=
# Exercise the NAND-boot socket path after a non-destructive CE RAM handoff.
COLD_SOCKET_TEST ?=
# Snapshot hardware registers at kernel entry and after driver setup.
HWDUMP ?=
# VEO_PROBE=1: Veo CF camera bring-up over USB diagnostics (docs/hardware/VEO.md).
VEO_PROBE ?=
# Beep at early kernel stages, before the LCD is on (power-on bring-up).
BOOT_BEEP ?=
# STOWAWAY_TRACE=1: log the dock keyboard's probe (modem lines, bytes) and every
# byte it sends, for bring-up over network debugging (tests/STOWAWAY.md).
STOWAWAY_TRACE ?=
# The idle thread's VR4131 STANDBY (docs/hardware/POWER.md); IDLE_STANDBY=0 spins
# instead, for bisecting on hardware. Rebuild kernel/sched.o after changing it.
IDLE_STANDBY ?= 1
# HIBERNATE_VERIFY=1: checksum every page in use across a HIBERNATE and log
# any the boot ROM or SPL overwrote on the way back (docs/hardware/POWER.md). Rebuild
# kernel/drivers/power.o after changing it.
HIBERNATE_VERIFY ?=

# GCC's VR41xx errata workarounds, off by default as in Linux's VR41xx
# builds: VR_FIX="-mfix-vr4130 -mfix-vr4120" pads mflo/mfhi and keeps GCC
# off DIV, calling the helpers in lib/libc/divdi3.c. Tried while chasing
# corruption on a real BE-300 that turned out to be shared-memory cache
# colours (kernel/shm.c); kept for testing.
VR_FIX ?=
ARCHFLAGS := -march=mips2 -mabi=32 -mno-abicalls -fno-pic -G 0 -msoft-float $(VR_FIX)
CFLAGS_COMMON := $(ARCHFLAGS) -std=gnu11 -ffreestanding -fno-builtin -nostdlib \
    -fno-stack-protector -Wall -Wextra -Wno-unused-parameter -g -Ilib/include -Ilib/libc/include

.DEFAULT_GOAL := all

# The build is split by area into mk/*.mk, included in this order.
include mk/libs.mk
include mk/programs.mk
include mk/assets.mk
include mk/romfs.mk
include mk/kernel.mk
include mk/spl.mk
include mk/packages.mk
include mk/fs.mk
include mk/nand.mk
include mk/glazeboot.mk

clean:
	rm -rf $(OUT)

FORCE:
.PHONY: all clean packages FORCE

-include $(shell find $(OUT) -name '*.d' 2>/dev/null)

include mk/release.mk
