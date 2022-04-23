#ifndef SYS80_H
#define SYS80_H

#include "hardware/pio.h"

#define SYS80_PIO pio1

extern uint8_t g_registers[256];

void InitSys80();

static inline bool IsCommandReady() {
  return !pio_sm_is_rx_fifo_empty(SYS80_PIO, 3);
}

static inline uint32_t UnloadCommand() {
  return pio_sm_get(SYS80_PIO, 3);
}

#endif  // SYS80_H
