# Runtime linker fixtures are test artifacts; only package with LD_TESTS=1.
LD_TEST_NAMES := dep plugin simple bss slow scope_dep scope_parent missing global consumer reentry cycle_a cycle_b
LD_TEST_SOS := $(foreach n,$(LD_TEST_NAMES),$(OUT)/ldtests/libld-$(n).so.1)
$(OUT)/ldtests/%.o: tests/dynamic/%.c
	@mkdir -p $(dir $@)
	$(CC) $(PIC_CFLAGS) -c -o $@ $<
$(OUT)/ldtests/libld-%.so.1: $(OUT)/ldtests/%.o $(OUT)/shared/libc.so.1
	$(LD) $(DYN_LDFLAGS) -shared -soname libld-$*.so.1 -o $@ $< -L$(OUT)/shared --as-needed -l:libc.so.1
$(OUT)/ldtests/libld-plugin.so.1: $(OUT)/ldtests/plugin.o $(OUT)/ldtests/libld-dep.so.1 $(OUT)/shared/libc.so.1
	$(LD) $(DYN_LDFLAGS) -shared -soname libld-plugin.so.1 -o $@ $< -L$(OUT)/ldtests -l:libld-dep.so.1 -L$(OUT)/shared -l:libc.so.1
$(OUT)/ldtests/cycle_seed.so: $(OUT)/ldtests/cycle_a.o
	$(LD) $(DYN_LDFLAGS) -shared -soname libld-cycle_a.so.1 -o $@ $<
$(OUT)/ldtests/libld-cycle_b.so.1: $(OUT)/ldtests/cycle_b.o $(OUT)/ldtests/cycle_seed.so
	$(LD) $(DYN_LDFLAGS) -shared -soname libld-cycle_b.so.1 -o $@ $< -L$(OUT)/ldtests -l:cycle_seed.so
$(OUT)/ldtests/libld-cycle_a.so.1: $(OUT)/ldtests/cycle_a.o $(OUT)/ldtests/libld-cycle_b.so.1
	$(LD) $(DYN_LDFLAGS) -shared -soname libld-cycle_a.so.1 -o $@ $< -L$(OUT)/ldtests -l:libld-cycle_b.so.1
ifeq ($(LD_TESTS),1)
ROMFS_LIBS += $(LD_TEST_SOS)
endif
$(OUT)/ldtests/libld-scope_parent.so.1: $(OUT)/ldtests/scope_parent.o $(OUT)/ldtests/libld-scope_dep.so.1
	$(LD) $(DYN_LDFLAGS) -shared -soname libld-scope_parent.so.1 -o $@ $< -L$(OUT)/ldtests -l:libld-scope_dep.so.1
