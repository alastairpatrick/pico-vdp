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
  uint8_t value;
  uint8_t track;
} TrackedSys80Reg;

typedef struct {
  uint16_t operation;
  uint16_t device;
  uint32_t address;
  uint16_t data;
  uint16_t pad[3];
} MemAccessSys80Regs;

enum {
  VIDEO_FLAG_PLANE_0_EN   = 0x01,
  VIDEO_FLAG_PLANE_1_EN   = 0x02,
  VIDEO_FLAG_TEXT_EN      = 0x04,
  VIDEO_FLAG_HIRES_EN     = 0x08,
  VIDEO_FLAG_TEXT_ROWS    = 0x10,
  VIDEO_FLAG_SPRITE_EN    = 0x40,
  VIDEO_FLAG_SPRITE_PRI   = 0x80,
};

typedef struct {
  uint16_t window_x, window_y;
  uint16_t pad[6];
} PlaneRegs;

typedef struct {
  // Read-write registers
  union {
    struct {
      // Audio / PIO range $0x, $1x
      TrackedSys80Reg ay[2][16];

      // Video generator range $2x
      uint16_t video_flags;           // $20
      uint16_t pad[7];
      MemAccessSys80Regs mem_access;  // $28
      PlaneRegs plane_regs[2];        // $30
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
      uint16_t pad2[13];

      // Video generator range $Ax, $Bx
      uint16_t current_y;            // $A0

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

static inline int STRIPED_SECTION Unswizzle16BitSys80Reg(uint32_t data) {
  return ((data >> 8) | data) & 0xFFFF;
}

#endif  // SYS80_H
