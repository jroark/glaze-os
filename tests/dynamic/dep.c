extern void ldtest_event(int);
int dep_value = 11;
int dep_add(int n) { return n + dep_value; }
__attribute__((constructor)) static void init(void) { ldtest_event(1); }
__attribute__((destructor)) static void fini(void) { ldtest_event(-1); }
