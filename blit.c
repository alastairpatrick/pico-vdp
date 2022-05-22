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

#define NUM_BLIT_REGS 8
#define LOCAL_BANK_SIZE (128 * 1024 / sizeof(uint32_t))
#define MCYCLE_TIME 32

typedef enum {
  OPCODE_SET0,
  OPCODE_SET1,
  OPCODE_SET2,
  OPCODE_SET3,
  OPCODE_SET4,
  OPCODE_SET5,
  OPCODE_SET6,
  OPCODE_SET7,
  OPCODE_SWAP       = 16,
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

static int STRIPED_SECTION MakeAddress(int low8, int high16) {
  return (low8 & 0xFF) | ((high16 & 0xFFFF) << 8);
}

static uint32_t* STRIPED_SECTION AccessVRAM(int addr) {
  if (addr < DISPLAY_BANK_SIZE*2) {
    return &g_blit_bank->words[addr & (DISPLAY_BANK_SIZE-1)];
  } else {
    return &g_local_bank[(addr - DISPLAY_BANK_SIZE*2) & (LOCAL_BANK_SIZE-1)];
  }
}

static uint32_t STRIPED_SECTION ReadVRAM(int addr) {
  return *AccessVRAM(addr);
}

static void STRIPED_SECTION WriteVRAM(int addr, uint32_t data) {
  *AccessVRAM(addr) = data;
}

static void STRIPED_SECTION DoSet(int reg_idx, int value) {
  g_blit_regs[reg_idx & (NUM_BLIT_REGS-1)] = value;
}

static void STRIPED_SECTION DoStream(int low8, int high16) {
  int addr = MakeAddress(low8, high16);
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

static void STRIPED_SECTION DoClear(int low8, int high16) {
  int addr = MakeAddress(low8, high16);
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

static void STRIPED_SECTION DoSwap(int swap_mode) {
  SwapBanks((SwapMode) swap_mode);
  while (IsSwapPending()) {
    MCycle();
  }
  g_blit_bank = GetBlitBank();
}

static void STRIPED_SECTION HandleShortCommand(int opcode, int operand) {
  switch (opcode) {
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
    DoStream(s_operand, l_operand);
    break;
  case OPCODE_CLEAR:
    DoClear(s_operand, l_operand);
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
