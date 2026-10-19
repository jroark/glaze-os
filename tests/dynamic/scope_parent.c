extern int scope_marker(void);
int scope_value(void) { return 20; }
int scope_parent(void) { return scope_marker(); }
