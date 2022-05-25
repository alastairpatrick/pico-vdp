#ifndef SYS80_H
#define SYS80_H

#include "hardware/pio.h"

#include "section.h"

#define KEYBOARD_ROWS 16

#define SYS80_PIO pio1

typedef struct {
  // Read-write registers
  union {
    struct {
      uint8_t dummy[32];
      uint8_t border_rgb;           // $20
      uint8_t border_left: 4;       // $21
      uint8_t border_right: 4;
      uint8_t sprite_period;        // $22
      uint8_t sprite_duty;          // $23
      uint8_t sprite_x;             // $24
      uint8_t sprite_y;             // $25
      uint8_t sprite_rgb;           // $26
      uint16_t sprite_bitmap[8];    // $27
    };
    uint8_t rw_bytes[128];
  };

  // Read-only registers
  union {
    struct {
      uint8_t kbd_rows[KEYBOARD_ROWS];
      uint8_t kbd_modifiers;
      uint8_t mouse_x, mouse_y;
      uint8_t mouse_buttons;
    };
    uint8_t ro_bytes[128];
  };
} Sys80Registers;

static_assert(sizeof(Sys80Registers) == 256);

extern volatile Sys80Registers g_sys80_regs;

void InitSys80();

static inline bool STRIPED_SECTION IsFifoEmpty() {
  return pio_sm_is_rx_fifo_empty(SYS80_PIO, 3);
}

static inline int STRIPED_SECTION PopFifo() {
  return pio_sm_get(SYS80_PIO, 3);
}

#endif  // SYS80_H
