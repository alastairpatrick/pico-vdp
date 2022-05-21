#include <assert.h>
#include <stdlib.h>
#include "pico/stdlib.h"

#include "blit.h"

#include "scan_out.h"
#include "sys80.h"

#define NUM_BLIT_REGS 8
#define LOCAL_BANK_SIZE (128 * 1024 / sizeof(uint32_t))

typedef enum {
  OPCODE_SET0,
  OPCODE_SET1,
  OPCODE_SET2,
  OPCODE_SET3,
  OPCODE_SET4,
  OPCODE_SET5,
  OPCODE_SET6,
  OPCODE_SET7,
  OPCODE_NOP        = 0b011111,

  OPCODE_SETN,
  OPCODE_STREAM,
} Opcode;

typedef struct {
  Opcode opcode: 6;
  int16_t operand: 10;
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

static int16_t g_blit_regs[NUM_BLIT_REGS];
static uint32_t g_local_bank[LOCAL_BANK_SIZE];

static inline int MakeAddress(int low8, int high16) {
  return (low8 & 0xFF) | ((high16 & 0xFFFF) << 8);
}

static inline void DoStream(int low8, int high16) {
  int size = g_blit_regs[0];
  int dest_addr = MakeAddress(low8, high16);

  for (int i = 0; i < size; ++i) {
    uint32_t data = PopFifo();

    if (dest_addr < DISPLAY_BANK_SIZE*2) {
      g_blit_bank->words[dest_addr & (DISPLAY_BANK_SIZE-1)] = data;
    }
    
    g_local_bank[(dest_addr - DISPLAY_BANK_SIZE*2) & (LOCAL_BANK_SIZE-1)] = data;
    ++dest_addr;
  }
}

static void HandleShortCommand(int opcode, int operand) {
  switch (opcode) {
  case OPCODE_SET0:
  case OPCODE_SET1:
  case OPCODE_SET2:
  case OPCODE_SET3:
  case OPCODE_SET4:
  case OPCODE_SET5:
  case OPCODE_SET6:
  case OPCODE_SET7:
    g_blit_regs[opcode] = operand;
    break;
  default:
    break;
  }
}

static void HandleLongCommand(Opcode opcode, int s_operand, int l_operand) {
  switch (opcode) {
  case OPCODE_SETN:
    g_blit_regs[s_operand & (NUM_BLIT_REGS-1)] = l_operand;
    break;
  case OPCODE_STREAM:
    DoStream(s_operand, l_operand);
    break;
  default:
    break;
  }
}

void BlitMain() {
  for (;;) {
    union {
      uint32_t word;
      LongCommand cmd;
    } u;
    static uint32_t foo;
    foo = u.word = PopFifo();
    if (u.cmd.short0.opcode <= OPCODE_NOP) {
      HandleShortCommand(u.cmd.short0.opcode, u.cmd.short0.operand);
      HandleShortCommand(u.cmd.short1.opcode, u.cmd.short1.operand);
    } else {
      HandleLongCommand(u.cmd.short0.opcode, u.cmd.short0.operand, u.cmd.long_operand);
    }
  }
}
