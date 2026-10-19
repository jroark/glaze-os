extern int cycle_b(void);
int cycle_a_global = 19;
int cycle_a(void) { return cycle_b() + 1; }
