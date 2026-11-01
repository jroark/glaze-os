# ---- NAND image -------------------------------------------------------------

# romfs sits raw in NAND after the kernel, not in the kernel image,
# and the kernel pages it in (kernel/romfs.c).
MK_NAND_VERSION := --version '$(GLAZE_VERSION)' --build '$(BUILD_ID)'
$(OUT)/glaze.bin: $(OUT)/spl.elf $(OUT)/kernel.elf $(OUT)/romfs.img $(OUT)/fs.img tools/mk_nand.py VERSION
	$(PYTHON) tools/mk_nand.py --kernel $(OUT)/kernel.elf --spl $(OUT)/spl.elf $(MK_NAND_VERSION) \
	    --romfs $(OUT)/romfs.img --fs-image $(OUT)/fs.img --out $@

all: $(OUT)/glaze.bin $(PKG_FILES)
