#include <mini/mini.h>
#include "mini_config.h"
#if MINI_WITH_EXTRA
int mini_extra(void);
#else
static int mini_extra(void) { return 0; }
#endif
#ifndef MINI_HAVE_STDINT
#error "stdint check"
#endif
int mini_value(void) { return MINI_SCALE * 100 + MINI_DOUBLE + MINI_VERSION_MAJOR * 10000 + mini_extra(); }
