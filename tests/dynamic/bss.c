char large_private[128 * 1024];
int bss_value(void) { return large_private[sizeof(large_private) - 1]; }
