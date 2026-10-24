/* Glaze has no hardware floating-point environment. JavaScript uses nearest-even. */
#include "fp_mode.h"
CRT_FE_ROUND_MODE __fe_getround(void) { return CRT_FE_TONEAREST; }
int __fe_raise_inexact(void) { return 0; }
