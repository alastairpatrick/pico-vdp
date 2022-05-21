#ifndef SYS80_H
#define SYS80_H

#include "hardware/pio.h"

#define SYS80_PIO pio1

typedef union {
  struct {
    uint8_t dummy[32];
    uint8_t border_rgb;
    uint8_t border_left: 4;
    uint8_t border_right: 4;
  };
  uint8_t bytes[256];
} Sys80Registers;

static_assert(sizeof(Sys80Registers) == 256);

extern volatile Sys80Registers g_sys80_regs;

void InitSys80();

static inline uint32_t PopFifo() {
  return pio_sm_get_blocking(SYS80_PIO, 3);
}

#endif  // SYS80_H
