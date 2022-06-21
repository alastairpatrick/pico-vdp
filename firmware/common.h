#ifndef COMMON_H
#define COMMON_H

#include "pins.h"
#include "section.h"

#if PICOVDP_OVERCLOCK
#define OVERCLOCK_SELECT(a, b) (b)
#else
#define OVERCLOCK_SELECT(a, b) (a)
#endif

#endif  // COMMON_H