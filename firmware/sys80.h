#ifndef SYS80_H
#define SYS80_H

#include "hardware/pio.h"

#include "section.h"

#define KEYBOARD_ROWS 11

enum {
  LED_CAPS_LOCK_MASK = 0x40,
  LED_NUM_LOCK_MASK = 0x20,
  LED_SCROLL_LOCK_MASK = 0x10,
};

typedef struct {
  // Read-write registers
  union {
    struct {
      // Audio / PIO range $0x, $1x
      struct {
        uint8_t value;
        uint8_t track;
      } ay[16];
      uint16_t pad4[15];
      uint16_t leds;                 // $1F

      // Video generator range $2x, $3x
      uint16_t lines_page;           // $20
      uint16_t start_line;           // $21
      uint16_t wrap_line;            // $22
      uint16_t border_rgb;           // $23
      uint16_t border_left: 4;       // $24
      uint16_t border_right: 4;
      uint16_t pad[6];
      uint16_t sprite_x;             // $2B
      uint16_t sprite_y;             // $2C
      uint16_t sprite_period;        // $2D
      uint16_t sprite_duty;          // $2E
      uint16_t sprite_rgb;           // $2F
      uint16_t sprite_bitmap[16];    // $30

      // Blitter range $4x
      uint16_t fifo_wrap;            // $40
    };
    uint16_t rw_bytes[128];
  };

  // Read-only registers
  union {
    struct {
      // Audio / PIO range $8x, $9x
      uint16_t kbd_rows[KEYBOARD_ROWS];  // $80
      uint16_t kbd_dummy[16-KEYBOARD_ROWS];
      uint16_t mouse_x, mouse_y;     // $90, $91
      uint16_t mouse_buttons;        // $92
      uint16_t pad2[14];

      // Video generator range $Ax, $Bx
      uint16_t current_y;            // $A0
      uint16_t pad3[31];

      // Blitter range $Cx
      uint16_t blit_sync;            // $C0
    };
    uint16_t ro_bytes[128];
  };
} Sys80Registers;

static_assert(sizeof(Sys80Registers) == 512);

extern volatile Sys80Registers g_sys80_regs;

void InitSys80();

static inline bool STRIPED_SECTION IsSys80FifoEmpty() {
  return pio_sm_is_rx_fifo_empty(pio1, 3);
}

static inline uint32_t STRIPED_SECTION PopSys80Fifo() {
  return pio_sm_get(pio1, 3);
}

static inline uint32_t STRIPED_SECTION Swizzle16BitSys80Reg(int data) {
  return (data & 0xFF) | ((data & 0xFF00) << 8);
}

#endif  // SYS80_H
