#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "pico/stdlib.h"

#include "blit.h"

#include "perf.h"
#include "pins.h"
#include "scan_out.h"
#include "section.h"
#include "sys80.h"
#include "video_dma.h"

#define NUM_BLIT_REGS 16
#define BLITTER_BANK_SIZE (128 * 1024 / sizeof(uint32_t))

#define OPTIMIZE

#ifdef OPTIMIZE
#define MCYCLE_TIME 16
#else
#define MCYCLE_TIME 64
#endif

enum {
  BLIT_FLAG_BLEND         = 0x0007,
  BLIT_FLAG_BLEND_REPLACE = 0x0000,
  BLIT_FLAG_BLEND_AND     = 0x0001,
  BLIT_FLAG_BLEND_OR      = 0x0002,
  BLIT_FLAG_BLEND_XOR     = 0x0003,
  BLIT_FLAG_BLEND_ADD     = 0x0004,
  BLIT_FLAG_BLEND_SUB     = 0x0005,
  BLIT_FLAG_BLEND_ADDS    = 0x0006,
  BLIT_FLAG_BLEND_SUBS    = 0x0007,

  BLIT_FLAG_UNPACK        = 0x0300,
  BLIT_FLAG_UNPACK_OFF    = 0x0000,
  BLIT_FLAG_UNPACK_8_16   = 0x0100,
  BLIT_FLAG_UNPACK_8_32   = 0x0200,
  BLIT_FLAG_UNPACK_16_32  = 0x0300,

  BLIT_FLAG_MASKED        = 0x0400,
};

enum {
  BLIT_REG_DEST_ADDR      = 0,  // Destination address
  BLIT_REG_SRC_ADDR       = 1,  // Source address
  BLIT_REG_STACK_ADDR     = 2,  // Top of stack in blitter bank
  BLIT_REG_COUNT          = 4,  // Iteration count or count pair.
  BLIT_REG_FLAGS          = 5,  // Miscellaneous flags.
  BLIT_REG_COLORS         = 6,  // Array of colors.
  BLIT_REG_CLIP           = 7,  // Left side of unclipped area.
  BLIT_REG_PITCH          = 8,  // Offset added to display address.
  BLIT_REG_GUARD          = 9,  // 2KB display bank pages that are read-only.
  BLIT_REG_SYNC           = 15, // Does not affect blitter.
};

enum {
  BLIT_OP_FLAGS_EN        = 0x01,
  BLIT_OP_CLIP_EN         = 0x02,
  BLIT_OP_COLOR_EN        = 0x04,

  BLIT_OP_TOPY            = 0x08,
  BLIT_OP_TOPY_PLAN       = 0x08,
  BLIT_OP_TOPY_LIN        = 0x00,

  BLIT_OP_SRC             = 0x30,
  BLIT_OP_SRC_DISPLAY     = 0x00,
  BLIT_OP_SRC_BLITTER     = 0x10,
  BLIT_OP_SRC_ZERO        = 0x20,
  BLIT_OP_SRC_STREAM      = 0x30,

  BLIT_OP_DEST            = 0xC0,
  BLIT_OP_DEST_COLORS     = 0x40,
  BLIT_OP_DEST_DISPLAY    = 0x80,
  BLIT_OP_DEST_BLITTER    = 0xC0,
};

// 0000dddd - SET dddd, #imm
// 00010sss - PUSH sss
// 00011ddd - POP ddd
// 0010ssdd - MOVE ss, dd
// 001110nn - SWAP nn
// 01xxxxxx - Blit operation, destination COLORS register
// 10xxxxxx - Blit operation, destination display bank
// 11xxxxxx - Blit operation, destination blitter bank
typedef enum {
  OPCODE_SET        = 0x00,
  OPCODE_PUSH       = 0x10,
  OPCODE_POP        = 0x18,
  OPCODE_MOVE       = 0x20,
  OPCODE_NOP        = 0x20,   // MOVE R0, R0
  OPCODE_SWAP       = 0x38,
  OPCODE_BLIT_BASE  = 0x40,

  OPCODE_DSAMPLE    = BLIT_OP_SRC_DISPLAY | BLIT_OP_DEST_COLORS  | BLIT_OP_TOPY_PLAN,                                                          // 0x48

  OPCODE_DCLEAR     = BLIT_OP_SRC_ZERO    | BLIT_OP_DEST_DISPLAY | BLIT_OP_TOPY_LIN                                       | BLIT_OP_COLOR_EN,  // 0xA4
  OPCODE_DDCOPY     = BLIT_OP_SRC_DISPLAY | BLIT_OP_DEST_DISPLAY | BLIT_OP_TOPY_PLAN |                    BLIT_OP_CLIP_EN,                     // 0x8A
  OPCODE_DSTREAM    = BLIT_OP_SRC_STREAM  | BLIT_OP_DEST_DISPLAY | BLIT_OP_TOPY_LIN,                                                           // 0xB0
  OPCODE_BSTREAM    = BLIT_OP_SRC_STREAM  | BLIT_OP_DEST_BLITTER | BLIT_OP_TOPY_LIN,                                                           // 0xF0
  OPCODE_RECT       = BLIT_OP_SRC_ZERO    | BLIT_OP_DEST_DISPLAY | BLIT_OP_TOPY_PLAN |                                      BLIT_OP_COLOR_EN,  // 0xAC
  OPCODE_IMAGE      = BLIT_OP_SRC_BLITTER | BLIT_OP_DEST_DISPLAY | BLIT_OP_TOPY_PLAN | BLIT_OP_FLAGS_EN | BLIT_OP_CLIP_EN | BLIT_OP_COLOR_EN,  // 0x9F
} Opcode;

typedef struct {
  uint64_t bits;
  int size;
} Fifo64;

static DisplayBank* g_blit_display_bank;
static uint16_t g_blit_regs[NUM_BLIT_REGS];

static int g_last_dot_x;
static int64_t g_scan_dot_cycle, g_blit_dot_cycle;

static union {
  uint8_t words8[BLITTER_BANK_SIZE*4];
  uint16_t words16[BLITTER_BANK_SIZE*2];
  uint32_t words32[BLITTER_BANK_SIZE];
} g_blit_bank;

static Fifo64 g_cmd_fifo;
static int g_fifo_begin, g_fifo_end;

static uint16_t g_unpack12[0x100];
static uint16_t g_unpack24[0x100];

#pragma GCC push_options

#ifdef OPTIMIZE
#pragma GCC optimize("O3")
#else
#pragma GCC optimize("Og")
#endif

static STRIPED_SECTION int Max(int a, int b) {
  return a > b ? a : b;
}

static STRIPED_SECTION int Min(int a, int b) {
  return a < b ? a : b;
}

static STRIPED_SECTION void Fifo64Push(Fifo64* fifo, uint32_t bits, int n) {
  bits <<= 32 - n;
  bits >>= 32 - n;
  fifo->bits |= ((uint64_t) bits) << ((uint64_t) fifo->size);
  fifo->size += n;
  //assert(fifo->size <= 64);
}

static STRIPED_SECTION int Fifo64Pop(Fifo64* fifo, int n) {
  int bits = fifo->bits & ((1 << n) - 1);
  fifo->bits >>= n;
  fifo->size -= n;
  //assert(fifo->size >= 0);
  return bits;
}

// 32-bit word offset as addr.
static uint32_t STRIPED_SECTION ReadBlitterBank(unsigned addr) {
  return g_blit_bank.words32[addr & (BLITTER_BANK_SIZE-1)];
}

static void STRIPED_SECTION WriteBlitterBank(unsigned addr, uint32_t data) {
  g_blit_bank.words32[addr & (BLITTER_BANK_SIZE-1)] = data;
}

static void STRIPED_SECTION MCycle(int cycles) {
  g_blit_dot_cycle += MCYCLE_TIME * cycles;
  assert(g_scan_dot_cycle - g_blit_dot_cycle < g_total_logical_width * 2);

  int dot_x;
  do {
    do {
      dot_x = GetDotX();
      int elapsed = dot_x - g_last_dot_x;
      if (elapsed < 0) {
         elapsed += g_total_logical_width;
      }

      g_last_dot_x = dot_x;
      g_scan_dot_cycle += elapsed;
    } while (g_blit_dot_cycle > g_scan_dot_cycle);

    int fifo_and = (1 << g_sys80_regs.fifo_wrap) - 1;

    if (!IsSys80FifoEmpty() && ((g_fifo_end + 1) & fifo_and) != g_fifo_begin) {
      g_blit_dot_cycle += MCYCLE_TIME;

      uint32_t data = PopSys80Fifo();
      WriteBlitterBank(g_fifo_end, data);

      g_fifo_end = (g_fifo_end + 1) & fifo_and;

      continue;
    }

    if (!IsBlitClockEnabled(dot_x)) {
      g_blit_dot_cycle += MCYCLE_TIME;
      continue;
    }
    
  } while (false);
}

static int STRIPED_SECTION PopCmdFifo(int n) {
  for (;;) {
    if (g_cmd_fifo.size >= n) {
      return Fifo64Pop(&g_cmd_fifo, n);
    }
    
    if (g_fifo_end != g_fifo_begin) {
      Fifo64Push(&g_cmd_fifo, ReadBlitterBank(g_fifo_begin), 32);

      int fifo_and = (1 << g_sys80_regs.fifo_wrap) - 1;
      g_fifo_begin = (g_fifo_begin + 1) & fifo_and;
    } else if (!IsSys80FifoEmpty()) {
      // This path is needed used the external FIFO is disabled, i.e. fifo_and == 0.
      uint32_t data = PopSys80Fifo();
      Fifo64Push(&g_cmd_fifo, data, 32);
    }

    MCycle(1);
  }
}

static int STRIPED_SECTION GetRegister(int idx) {
  return g_blit_regs[idx & (NUM_BLIT_REGS-1)];
}

static void STRIPED_SECTION SetRegister(int idx, int data) {
  idx &= (NUM_BLIT_REGS-1);  
  g_blit_regs[idx] = data;

  // Read only view for CPU.
  g_sys80_regs.blit[idx] = Swizzle16BitSys80Reg(data);
}

static void STRIPED_SECTION PushRegister(int idx) {
  int top = g_blit_regs[BLIT_REG_STACK_ADDR];
  SetRegister(BLIT_REG_STACK_ADDR, top + 1);
  MCycle(1);
  WriteBlitterBank(top, GetRegister(idx));
}

static void STRIPED_SECTION PopRegister(int idx) {
  int top = g_blit_regs[BLIT_REG_STACK_ADDR] - 1;
  SetRegister(idx, ReadBlitterBank(top));
  SetRegister(BLIT_REG_STACK_ADDR, top);
  MCycle(1);
}

static void STRIPED_SECTION MoveRegister(int dest, int source) {
  SetRegister(dest, GetRegister(source));
}

// Packed variants all use byte offsets as addr.
static int STRIPED_SECTION ReadPackedBlitterBank8(unsigned addr) {
  return g_blit_bank.words8[addr & (BLITTER_BANK_SIZE*4-1)];
}

static int STRIPED_SECTION ReadPackedBlitterBank16(unsigned addr) {
  return g_blit_bank.words16[(addr/2) & (BLITTER_BANK_SIZE*2-1)];
}

static uint32_t STRIPED_SECTION ReadPackedBlitterBank32(unsigned addr) {
  return g_blit_bank.words32[(addr/4) & (BLITTER_BANK_SIZE-1)];
}

static uint32_t STRIPED_SECTION ReadDisplayBank(unsigned addr) {
  return g_blit_display_bank->words[addr & (DISPLAY_BANK_SIZE-1)];
}

static void STRIPED_SECTION WriteDisplayBank(unsigned addr, uint32_t data) {
  int page = addr >> 9;
  int guard = g_blit_regs[BLIT_REG_GUARD];
  if (((guard >> page) & 1) == 0) {
    g_blit_display_bank->words[addr & (DISPLAY_BANK_SIZE-1)] = data;
  }
}

typedef uint32_t (*ReadSourceDataFn)(int daddr, int* baddr_byte);

uint32_t STRIPED_SECTION ReadBlitterSourceData(int daddr, int* baddr_byte) {
  uint32_t data32 = ReadPackedBlitterBank32(*baddr_byte);
  *baddr_byte += 4;
  return data32;
}

uint32_t STRIPED_SECTION UnpackBlitterSourceData_16_32(int daddr, int* baddr_byte) {
  uint32_t data16 = ReadPackedBlitterBank16(*baddr_byte);
  uint32_t data32 = g_unpack24[data16 & 0xFF] | (g_unpack24[data16 >> 8] << 16);
  *baddr_byte += 2;
  return data32;
}

uint32_t STRIPED_SECTION UnpackBlitterSourceData_8_32(int daddr, int* baddr_byte) {
  int data8 = ReadPackedBlitterBank8(*baddr_byte);
  uint32_t data16 = g_unpack12[data8];
  uint32_t data32 = g_unpack24[data16 & 0xFF] | (g_unpack24[data16 >> 8] << 16);
  *baddr_byte += 1;
  return data32;
}

uint32_t STRIPED_SECTION UnpackBlitterSourceData_8_16(int daddr, int* baddr_byte) {
  uint32_t data8 = ReadPackedBlitterBank8(*baddr_byte);
  uint32_t data16 = g_unpack12[data8];
  *baddr_byte += 1;
  return data16 | (data16 << 16);
}

static ReadSourceDataFn STRIPED_SECTION PrepareUnpackSourceData(Opcode opcode) {
  int flags = g_blit_regs[BLIT_REG_FLAGS];
  if (!(opcode & BLIT_OP_FLAGS_EN)) {
    flags = BLIT_FLAG_UNPACK_OFF;
  }

  switch (flags & BLIT_FLAG_UNPACK) {
    case BLIT_FLAG_UNPACK_OFF:
      return ReadBlitterSourceData;
    case BLIT_FLAG_UNPACK_16_32:
      return UnpackBlitterSourceData_16_32;
    case BLIT_FLAG_UNPACK_8_32:
      return UnpackBlitterSourceData_8_32;
    case BLIT_FLAG_UNPACK_8_16:
      return UnpackBlitterSourceData_8_16;
    //default:
      //assert(false);
  }
}

uint32_t STRIPED_SECTION ReadDisplayBankSourceData(int daddr, int* baddr_byte) {
  return ReadDisplayBank(daddr);
}

uint32_t STRIPED_SECTION ReadZeroSourceData(int daddr, int* baddr_byte) {
  return 0;
}

uint32_t STRIPED_SECTION ReadStreamSourceData(int daddr, int* baddr_byte) {
  return PopCmdFifo(32);
}

static ReadSourceDataFn STRIPED_SECTION PrepareReadSourceData(Opcode opcode) {
  switch (opcode & BLIT_OP_SRC) {
  case BLIT_OP_SRC_BLITTER:
    return PrepareUnpackSourceData(opcode);
  case BLIT_OP_SRC_DISPLAY:
    return ReadDisplayBankSourceData;
  case BLIT_OP_SRC_ZERO:
    return ReadZeroSourceData;
  case BLIT_OP_SRC_STREAM:
    return ReadStreamSourceData;
  //default:
    //assert(false);
  }
}

static uint32_t STRIPED_SECTION ReadDestData(Opcode opcode, int daddr, int baddr) {
  switch (opcode & BLIT_OP_DEST) {
  case BLIT_OP_DEST_BLITTER:
    return ReadBlitterBank(baddr);
  case BLIT_OP_DEST_DISPLAY:
    return ReadDisplayBank(daddr);
  default:
    return g_blit_regs[BLIT_REG_COLORS];
  }
}

static void STRIPED_SECTION WriteDestData(Opcode opcode, int daddr, int baddr, uint32_t data, uint32_t mask) {
  switch (opcode & BLIT_OP_DEST) {
  case BLIT_OP_DEST_BLITTER:
    WriteBlitterBank(baddr, (ReadBlitterBank(baddr) & ~mask) | (data & mask));
    break;
  case BLIT_OP_DEST_DISPLAY:
    WriteDisplayBank(daddr, (ReadDisplayBank(daddr) & ~mask) | (data & mask));
    break;
  default:
    SetRegister(BLIT_REG_COLORS, (g_blit_regs[BLIT_REG_COLORS] & ~mask) | (data & mask));
    break;
  }
}

static void STRIPED_SECTION Overlap(int* begin, int* end, int x, int width) {
  *begin = Max(0, -x);
  *end = Min(32, width - x);
}

static uint32_t STRIPED_SECTION Parallel8(int v) {
  v = v | (v << 4);
  v = v | (v << 8);
  v = v | (v << 16);
  return v;
}

static uint32_t STRIPED_SECTION ParallelAdd(uint32_t a8, uint32_t b8) {
  uint32_t r8 = 0;
  for (int i = 0; i < 32; i += 4) {
    int a = (a8 >> i) & 0xF;
    int b = (b8 >> i) & 0xF;
    int r = (a + b) & 0xF;
    r8 |= r << i;
  }
  return r8;
}

// These ParallelX(a,b) functions could be optimized more but there's no reason
// at the time of writing. They are only used when blending is enabled and in that
// case, there is an additional m-cycle.

static uint32_t STRIPED_SECTION ParallelSub(uint32_t a8, uint32_t b8) {
  uint32_t r8 = 0;
  for (int i = 0; i < 32; i += 4) {
    int a = (a8 >> i) & 0xF;
    int b = (b8 >> i) & 0xF;
    int r = (a - b) & 0xF;
    r8 |= r << i;
  }
  return r8;
}

static uint32_t STRIPED_SECTION ParallelAddSat(uint32_t a8, uint32_t b8) {
  uint32_t r8 = 0;
  for (int i = 0; i < 32; i += 4) {
    int a = (a8 >> i) & 0xF;
    int b = (b8 >> i) & 0xF;
    int r = Min(0xF, (a + b));
    r8 |= r << i;
  }
  return r8;
}

static uint32_t STRIPED_SECTION ParallelSubSat(uint32_t a8, uint32_t b8) {
  uint32_t r8 = 0;
  for (int i = 0; i < 32; i += 4) {
    int a = (a8 >> i) & 0xF;
    int b = (b8 >> i) & 0xF;
    int r = Max(0, (a - b));
    r8 |= r << i;
  }
  return r8;
}

#define FUNC_NAME DoBlit
#define SRC_ZERO false
#define BASE_ITERATION_CYLES 2
#include "blittemp.h"
#undef FUNC_NAME
#undef SRC_ZERO
#undef BASE_ITERATION_CYLES

#define FUNC_NAME DoBlitSrcZero
#define SRC_ZERO true
#define BASE_ITERATION_CYLES 1
#include "blittemp.h"
#undef FUNC_NAME
#undef SRC_ZERO
#undef BASE_ITERATION_CYLES

#pragma GCC pop_options

static void STRIPED_SECTION DoSwap(SwapMode mode) {
  int line_idx = g_blit_regs[BLIT_REG_COUNT] & 0xFF;
  SwapBanks(mode, line_idx);

  while (IsSwapPending()) {
    MCycle(1);
  }

  g_blit_display_bank = GetBlitBank();
}

void InitBlit() {
  g_blit_display_bank = GetBlitBank();

  for (int packed = 0; packed < 0x100; ++packed) {
    for (int i = 0; i < 8; ++i) {
      int bits = (packed >> i) & 0x1;
      g_unpack12[packed] |= (bits << (i*2)) | (bits << (i*2+1));
    }
    for (int i = 0; i < 4; ++i) {
      int bits = (packed >> (i*2)) & 0x3;
      g_unpack24[packed] |= (bits << (i*4)) | (bits << (i*4+2));
    }
  }
}

void STRIPED_SECTION BlitMain() {
  InitBlit();
  g_last_dot_x = GetDotX();

  for (;;) {
    Opcode opcode = (Opcode) PopCmdFifo(8);
    MCycle(1);

    if (opcode < OPCODE_PUSH) {
      SetRegister(opcode, PopCmdFifo(16));
    } else if (opcode < OPCODE_POP) {
      // Do PUSH
      PushRegister(opcode & 0x7);
    } else if (opcode < OPCODE_MOVE) {
      // Do POP
      PopRegister(opcode & 0x7);
    } else if (opcode < OPCODE_SWAP) {
      // Do MOVE
      MoveRegister(opcode & 0x3, (opcode & 0xC0) >> 2);
    } else if (opcode < OPCODE_BLIT_BASE) {
      // Do SWAP
      DoSwap((SwapMode) (opcode & SWAP_MASK));
    } else {
      if ((opcode & BLIT_OP_SRC) == BLIT_OP_SRC_ZERO) {
        DoBlitSrcZero(opcode);
      } else {
        DoBlit(opcode);
      }
    }
  }
}
