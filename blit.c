#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "pico/stdlib.h"

#include "blit.h"

#include "pins.h"
#include "scan_out.h"
#include "section.h"
#include "sys80.h"
#include "video.h"

#define NUM_BLIT_REGS 16
#define LOCAL_BANK_SIZE (128 * 1024 / sizeof(uint32_t))
#define MCYCLE_TIME 16

enum {
  BLIT_XOR          = 0x0F,
  BLIT_UNZIP        = 0x70,
  BLIT_UNZIP_OFF    = 0x00,
  BLIT_UNZIP_2X     = 0x10,
  BLIT_UNZIP_4X     = 0x20,
  BLIT_MASK         = 0x80,
};

typedef enum {
  OPCODE_SET0,
  OPCODE_SET1,
  OPCODE_SET2,
  OPCODE_SET3,
  OPCODE_SET4,
  OPCODE_SET5,
  OPCODE_SET6,
  OPCODE_SET7,
  OPCODE_SET8,
  OPCODE_SET9,
  OPCODE_SET10,
  OPCODE_SET11,
  OPCODE_SET12,
  OPCODE_SET13,
  OPCODE_SET14,
  OPCODE_SET_PROT,
  OPCODE_BLIT,
  OPCODE_SAVE,
  OPCODE_RESTORE,
  OPCODE_SWAP       = 0b011110,
  OPCODE_NOP        = 0b011111,

  OPCODE_SETN,
  OPCODE_STREAM,
  OPCODE_CLEAR,
} Opcode;

typedef struct {
  Opcode opcode: 8;
  uint8_t operand;
} ShortCommand;

static_assert(sizeof(ShortCommand) == 2);

typedef struct {
  ShortCommand short0;
  union {
    ShortCommand short1;
    int16_t long_operand;
  };
} LongCommand;

static_assert(sizeof(LongCommand) == 4);

static DisplayBank* g_blit_bank;
static int16_t g_blit_regs[NUM_BLIT_REGS];
static uint32_t g_local_bank[LOCAL_BANK_SIZE];
static uint16_t g_time;

static void STRIPED_SECTION MCycle() {
  do {
    for (;;) {
      int current = GetDotTime();
      int target = g_time;
      int16_t diff = current - target;
      if (diff >= 0) {
        if (diff > 2000) {
          gpio_put(LED_PIN, true);  // For debugging
        }
        break;
      }
    }

    g_time = g_time + MCYCLE_TIME;
  } while (!g_blit_clock_enable);
}

static int STRIPED_SECTION GetRegister(int idx) {
  return g_blit_regs[idx & (NUM_BLIT_REGS-1)];
}

static void STRIPED_SECTION SetRegister(int idx, int data) {
  g_blit_regs[idx & (NUM_BLIT_REGS-1)] = data;
}

static uint32_t* STRIPED_SECTION AccessVRAM(unsigned addr) {
  if (addr < DISPLAY_BANK_SIZE*2) {
    return &g_blit_bank->words[addr & (DISPLAY_BANK_SIZE-1)];
  } else {
    return &g_local_bank[(addr - DISPLAY_BANK_SIZE*2) & (LOCAL_BANK_SIZE-1)];
  }
}

static uint32_t STRIPED_SECTION ReadVRAM(unsigned addr) {
  return *AccessVRAM(addr);
}

static void STRIPED_SECTION WriteVRAM(unsigned addr, uint32_t data) {
  *AccessVRAM(addr) = data;
}

static uint32_t STRIPED_SECTION ReadDisplayRAM(unsigned addr) {
  return g_blit_bank->words[addr & (DISPLAY_BANK_SIZE-1)];
}

static void STRIPED_SECTION WriteDisplayRAM(unsigned addr, uint32_t data) {
  g_blit_bank->words[addr & (DISPLAY_BANK_SIZE-1)] = data;
}

static void STRIPED_SECTION DoSet(unsigned reg_idx, int value) {
  SetRegister(reg_idx, value);
}

static void STRIPED_SECTION DoStream(unsigned addr) {
  int size = (uint16_t) g_blit_regs[0];
  int step = g_blit_regs[1];

  for (int i = 0; i < size; ++i) {
    do {
      MCycle();
    } while (IsFifoEmpty());

    uint32_t data = PopFifo();
    WriteVRAM(addr, data);
    addr += step;
  }
}

static void STRIPED_SECTION DoClear(unsigned addr) {
  int size = (uint16_t) g_blit_regs[0];
  int step = g_blit_regs[1];
  uint32_t data = (uint8_t) g_blit_regs[2];
  data = data | (data << 8) | (data << 16) | (data << 24);

  for (int i = 0; i < size; ++i) {
    MCycle();
    WriteVRAM(addr, data);
    addr += step;
  }
}

static void STRIPED_SECTION DoSave(unsigned reg_idx) {
  const int bits_per_pixel = 4;
  const int pixels_per_word = 32 / bits_per_pixel;
  const int pixel_mask = (1 << bits_per_pixel) - 1;

  int pixel_addr = g_blit_regs[0];
  int pitch = g_blit_regs[1];
  int blit_addr = g_blit_regs[2];
  int blit_width = g_blit_regs[3];
  int blit_height = g_blit_regs[4];
  int save_addr = g_blit_regs[7];
  SetRegister(reg_idx, save_addr);

  for (int y = 0; y < blit_height; ++y) {
    int display_addr = pixel_addr / pixels_per_word;

    int w = blit_width;
    if (pixel_addr & (pixels_per_word-1)) {
      ++w;
    }

    for (int i = 0; i < w; ++i) {
      MCycle();

      uint32_t data = ReadDisplayRAM(display_addr + i);
      WriteVRAM(--save_addr, data);
    }

    MCycle();
    WriteVRAM(--save_addr, display_addr | (w << 16));

    pixel_addr += pitch;
  }

  g_blit_regs[7] = save_addr;
}

static void STRIPED_SECTION DoRestore(unsigned reg_idx) {
  int save_addr = g_blit_regs[7];
  if (save_addr == 0) {
    save_addr = 0x10000;
  }

  int end_addr = GetRegister(reg_idx);
  if (end_addr == 0) {
    end_addr = 0x10000;
  }

  while (save_addr < end_addr) {
    MCycle();
    uint32_t header = ReadVRAM(save_addr++);
    int display_addr = header & 0xFFFF;
    int w = header >> 16;

    if (save_addr >= end_addr) {
      break;
    }

    for (int i = 0; i < w; ++i) {
      MCycle();

      uint32_t data = ReadVRAM(save_addr++);
      WriteDisplayRAM(display_addr + i, data);

      if (save_addr >= end_addr) {
        break;
      }
    }
  }

  g_blit_regs[7] = save_addr;
}

static void STRIPED_SECTION DoBlit(unsigned flags) {
  const int display_bits_per_pixel = 4;
  const int display_pixels_per_word = 32 / display_bits_per_pixel;
  const int display_pixel_mask = (1 << display_bits_per_pixel) - 1;

  int blit_xor = flags & BLIT_XOR;
  int unmasked = !(flags & BLIT_MASK);

  int blit_bits_per_pixel;
  switch (flags & BLIT_UNZIP) {
  case BLIT_UNZIP_4X:
    blit_bits_per_pixel = 1;
    break;
  case BLIT_UNZIP_2X:
    blit_bits_per_pixel = 2;
    break;
  default:
    blit_bits_per_pixel = 4;
    break;
  }

  int blit_pixel_mask = (1 << blit_bits_per_pixel) - 1;

  int pixel_addr = g_blit_regs[0];
  int pitch = g_blit_regs[1];
  int blit_addr = g_blit_regs[2];
  int blit_width = g_blit_regs[3];
  int blit_height = g_blit_regs[4];
  int clip_left = g_blit_regs[5] & 0xFF;
  int clip_right = (g_blit_regs[5] >> 8) & 0xFF;

  for (int y = 0; y < blit_height; ++y) {
    int display_addr = pixel_addr / display_pixels_per_word;

    uint32_t display_colors8 = ReadDisplayRAM(display_addr);
    int shift = (pixel_addr & (display_pixels_per_word-1)) * display_bits_per_pixel;

    int x = 0;
    for (int i = 0; i < blit_width; ++i) {
      uint32_t blit_colors = ReadVRAM(blit_addr++);
      for (int j = 0; j < 32; j += blit_bits_per_pixel) {
        int color = (blit_colors >> j) & blit_pixel_mask;
        if (color | unmasked) {
          if (x >= clip_left && x <= clip_right) {
            color ^= blit_xor;
            display_colors8 = (display_colors8 & ~(display_pixel_mask << shift)) | (color << shift);
          }
        }

        ++x;

        shift += display_bits_per_pixel;
        if (shift == 32) {
          MCycle();
          WriteDisplayRAM(display_addr++, display_colors8);
          display_colors8 = ReadDisplayRAM(display_addr);
          shift = 0;
        }
      }
    }

    if (shift) {
      MCycle();
      WriteDisplayRAM(display_addr, display_colors8);
    }

    pixel_addr += pitch;
  }
}

static void STRIPED_SECTION DoSwap(int swap_mode) {
  SwapBanks((SwapMode) swap_mode);
  while (IsSwapPending()) {
    MCycle();
  }
  g_blit_bank = GetBlitBank();
}

static void STRIPED_SECTION HandleShortCommand(int opcode, int operand) {
  switch (opcode) {
  case OPCODE_BLIT:
    DoBlit(operand);
    break;
  case OPCODE_RESTORE:
    DoRestore(operand);
    break;
  case OPCODE_SAVE:
    DoSave(operand);
    break;
  case OPCODE_SET0:
  case OPCODE_SET1:
  case OPCODE_SET2:
  case OPCODE_SET3:
  case OPCODE_SET4:
  case OPCODE_SET5:
  case OPCODE_SET6:
  case OPCODE_SET7:
    DoSet(opcode, operand);
    break;
  case OPCODE_SWAP:
    DoSwap(operand);
    break;
  default:
    break;
  }
}

static void STRIPED_SECTION HandleLongCommand(Opcode opcode, int s_operand, int l_operand) {
  switch (opcode) {
  case OPCODE_SETN:
    DoSet(s_operand, l_operand);
    break;
  case OPCODE_STREAM:
    DoStream(l_operand);
    break;
  case OPCODE_CLEAR:
    DoClear(l_operand);
    break;
  default:
    break;
  }
}

void STRIPED_SECTION BlitMain() {
  g_blit_bank = GetBlitBank();
  g_time = GetDotTime();

  for (;;) {
    do {
      MCycle();
    } while (IsFifoEmpty());

    union {
      uint32_t word;
      LongCommand cmd;
    } u;
    u.word = PopFifo();

    if (u.cmd.short0.opcode <= OPCODE_NOP) {
      HandleShortCommand(u.cmd.short0.opcode, u.cmd.short0.operand);
      HandleShortCommand(u.cmd.short1.opcode, u.cmd.short1.operand);
    } else {
      HandleLongCommand(u.cmd.short0.opcode, u.cmd.short0.operand, u.cmd.long_operand);
    }
  }
}
