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

#define NUM_BLIT_REGS 8
#define BLITTER_BANK_SIZE (128 * 1024 / sizeof(uint32_t))

#define OPTIMIZE

#ifdef OPTIMIZE
#define MCYCLE_TIME 16
#else
#define MCYCLE_TIME 64
#endif

enum {
  BLIT_FLAG_UNPACK        = 0x0300,
  BLIT_FLAG_UNPACK_OFF    = 0x0000,
  BLIT_FLAG_UNPACK_8_16   = 0x0100,
  BLIT_FLAG_UNPACK_8_32   = 0x0200,
  BLIT_FLAG_UNPACK_16_32  = 0x0300,
  BLIT_FLAG_MASKED        = 0x0400,
};

enum {
  BLIT_REG_DST_ADDR       = 0,  // Address of nibble in display bank.
  BLIT_REG_CLIP           = 1,  // Left side of unclipped area.
  BLIT_REG_SRC_ADDR       = 2,  // Address of a 32-bit word in blitter bank.
  BLIT_REG_COUNT          = 3,  // Iteration count or count pair.
  BLIT_REG_PITCH          = 4,  // Offset added to display address.
  BLIT_REG_FLAGS          = 5,  // Miscellaneous flags.
  BLIT_REG_COLORS         = 6,  // Array of colors.
};

enum {
  BLIT_OP_SRC             = 0x03,
  BLIT_OP_SRC_DISPLAY     = 0x00,
  BLIT_OP_SRC_BLITTER     = 0x01,
  BLIT_OP_SRC_ZERO        = 0x02,
  BLIT_OP_SRC_STREAM      = 0x03,

  BLIT_OP_DEST            = 0x04,
  BLIT_OP_DEST_DISPLAY    = 0x00,
  BLIT_OP_DEST_BLITTER    = 0x04,

  BLIT_OP_TOPY            = 0x08,
  BLIT_OP_TOPY_PLAN       = 0x00,
  BLIT_OP_TOPY_LIN        = 0x08,

  BLIT_OP_FLAGS_EN        = 0x10,
  BLIT_OP_CLIP_EN         = 0x20,
  BLIT_OP_COLOR_EN        = 0x40,
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

  OPCODE_BLIT_BASE  = 0x80,
 
  OPCODE_DCLEAR     = OPCODE_BLIT_BASE | BLIT_OP_SRC_ZERO    | BLIT_OP_DEST_DISPLAY | BLIT_OP_TOPY_LIN                                       | BLIT_OP_COLOR_EN,  // 0xCA
  OPCODE_DDCOPY     = OPCODE_BLIT_BASE | BLIT_OP_SRC_DISPLAY | BLIT_OP_DEST_DISPLAY | BLIT_OP_TOPY_PLAN |                    BLIT_OP_CLIP_EN,                     // 0xA0
  OPCODE_DSTREAM    = OPCODE_BLIT_BASE | BLIT_OP_SRC_STREAM  | BLIT_OP_DEST_DISPLAY | BLIT_OP_TOPY_LIN,                                                           // 0x8B
  OPCODE_BSTREAM    = OPCODE_BLIT_BASE | BLIT_OP_SRC_STREAM  | BLIT_OP_DEST_BLITTER | BLIT_OP_TOPY_LIN,                                                           // 0x8F
  OPCODE_RECT       = OPCODE_BLIT_BASE | BLIT_OP_SRC_ZERO    | BLIT_OP_DEST_DISPLAY | BLIT_OP_TOPY_PLAN |                                      BLIT_OP_COLOR_EN,  // 0xC2
  OPCODE_IMAGE      = OPCODE_BLIT_BASE | BLIT_OP_SRC_BLITTER | BLIT_OP_DEST_DISPLAY | BLIT_OP_TOPY_PLAN | BLIT_OP_FLAGS_EN | BLIT_OP_CLIP_EN | BLIT_OP_COLOR_EN,  // 0xF1

  OPCODE_SWAP0      = 0x48,
  OPCODE_SWAP1      = 0x49,
  OPCODE_SWAP2      = 0x4A,
  OPCODE_SWAP3      = 0x4B,
  OPCODE_NOP        = 0x4F,
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

    int fifo_begin = g_sys80_regs.fifo_begin;
    int fifo_end = g_sys80_regs.fifo_end;
    int fifo_and = (1 << g_sys80_regs.fifo_wrap) - 1;

    if (Sys80FifoLevel() >= 4 && ((fifo_end + 1) & fifo_and) != fifo_begin) {
      g_blit_dot_cycle += MCYCLE_TIME;

      uint32_t data = PopSys80Fifo() | (PopSys80Fifo() << 8) | (PopSys80Fifo() << 16) | (PopSys80Fifo() << 24);
      WriteBlitterBank(fifo_end, data);

      g_sys80_regs.fifo_end = fifo_end = (fifo_end + 1) & fifo_and;

      continue;
    }

    if (IsBlitClockEnabled(dot_x)) {
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
    
    int fifo_begin = g_sys80_regs.fifo_begin;
    int fifo_end = g_sys80_regs.fifo_end;
    if (fifo_end != fifo_begin) {
      Fifo64Push(&g_cmd_fifo, ReadBlitterBank(fifo_begin), 32);

      int fifo_and = (1 << g_sys80_regs.fifo_wrap) - 1;
      g_sys80_regs.fifo_begin = fifo_begin = (fifo_begin + 1) & fifo_and;
    }

    MCycle(1);
  }
}

static int STRIPED_SECTION GetRegister(int idx) {
  return g_blit_regs[idx & (NUM_BLIT_REGS-1)];
}

static void STRIPED_SECTION SetRegister(int idx, int data) {
  g_blit_regs[idx & (NUM_BLIT_REGS-1)] = data;
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
  g_blit_display_bank->words[addr & (DISPLAY_BANK_SIZE-1)] = data;
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

static uint32_t STRIPED_SECTION WriteDestData(Opcode opcode, int daddr, int baddr, uint32_t data, uint32_t mask) {
  switch (opcode & BLIT_OP_DEST) {
  case BLIT_OP_DEST_BLITTER:
    WriteBlitterBank(baddr, (ReadBlitterBank(baddr) & ~mask) | (data & mask));
  case BLIT_OP_DEST_DISPLAY:
    WriteDisplayBank(daddr, (ReadDisplayBank(daddr) & ~mask) | (data & mask));
  //default:
    //assert(false);
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

static PerfCounter g_blit_setup_perf;
static PerfCounter g_blit_cycle_perf[4];
static void STRIPED_SECTION DoBlit(Opcode opcode) {
  //DisableXIPCache();
  BeginPerf(&g_blit_setup_perf);

  int dest_daddr = g_blit_regs[BLIT_REG_DST_ADDR];
  int src_daddr = g_blit_regs[BLIT_REG_SRC_ADDR];
  int dest_baddr = g_blit_regs[BLIT_REG_DST_ADDR];
  int src_baddr_byte = g_blit_regs[BLIT_REG_SRC_ADDR] * 4;
  int flags = g_blit_regs[BLIT_REG_FLAGS];
  int pitch = (int16_t) g_blit_regs[BLIT_REG_PITCH];

  uint32_t mask_disable8 = 0xFFFFFFFF;
  if (opcode & BLIT_OP_FLAGS_EN) {
    mask_disable8 = flags & BLIT_FLAG_MASKED ? 0x00000000 : 0xFFFFFFFF;
  }

  uint32_t bg_color8 = 0x00000000;
  uint32_t fg_color8 = 0xFFFFFFFF;
  if (opcode & BLIT_OP_COLOR_EN) {
    bg_color8 = Parallel8(g_blit_regs[BLIT_REG_COLORS] & 0xF);
    fg_color8 = Parallel8((g_blit_regs[BLIT_REG_COLORS] >> 4) & 0xF);
  }

  int clip_left = 0;
  int clip_right = 0xFFFF;
  if (opcode & BLIT_OP_CLIP_EN) {
    int clip = g_blit_regs[BLIT_REG_CLIP];
    clip_left = clip & 0xFF;
    clip_right = clip >> 8;

    if (clip_right == 0) {
      clip_right = 0x100;
    }
  }

  int width, height;
  switch (opcode & BLIT_OP_TOPY) {
  case BLIT_OP_TOPY_LIN:
    width = g_blit_regs[BLIT_REG_COUNT];
    height = 1;
    break;
  case BLIT_OP_TOPY_PLAN:
    width = g_blit_regs[BLIT_REG_COUNT] & 0xFF;
    if (width == 0) {
      width = 0x100;
    }
    height = g_blit_regs[BLIT_REG_COUNT] >> 8;
    if (height == 0) {
      height = 0x100;
    }
    break;
  }

  bool src_planar = (opcode & BLIT_OP_SRC) == BLIT_OP_SRC_DISPLAY && (opcode & BLIT_OP_TOPY) == BLIT_OP_TOPY_PLAN;
  bool dest_planar = (opcode & BLIT_OP_DEST) == BLIT_OP_DEST_DISPLAY && (opcode & BLIT_OP_TOPY) == BLIT_OP_TOPY_PLAN;

  ReadSourceDataFn read_source_data = PrepareReadSourceData(opcode);

  Fifo64 fifo = {0};

  // x-coordinates are in bits beyond this point.
  clip_left *= 4;
  clip_right *= 4;
  width *= 4;

  src_daddr -= pitch;
  int src_daddr_word = 0;
  int src_x = width;
  int src_y = -1;
  dest_daddr -= pitch;
  int dest_daddr_word = 0;
  int dest_x = width;
  int dest_y = -1;

  EndPerf(&g_blit_setup_perf);

  for (;;) {
    BeginPerf(&g_blit_cycle_perf[opcode & BLIT_OP_SRC]);
    
    if (src_x >= width) {
      //MCycle(1);

      src_daddr += pitch;
      src_daddr_word = src_daddr >> 3;

      ++src_y;

      if (src_planar) {
        src_x = -(src_daddr & 0x7) * 4;
      } else {
        src_x = 0;
      }
    }

    if (fifo.size <= 32 && src_y < height) {
      uint32_t in_data = read_source_data(src_daddr_word, &src_baddr_byte);
      int begin, end;
      Overlap(&begin, &end, src_x, width);
      Fifo64Push(&fifo, in_data >> begin, end - begin);
      src_x += 32;
      ++src_daddr_word;
    }

    if (dest_x >= width) {
      dest_daddr += pitch;
      dest_daddr_word = dest_daddr >> 3;

      ++dest_y;
      if (dest_y == height) {
        //EnableXIPCache();
        return;
      }

      if (dest_planar) {
        dest_x = -(dest_daddr & 0x7) * 4;
      } else {
        dest_x = 0;
      }
    }

    {
      int begin, end;
      Overlap(&begin, &end, dest_x, width);
      int num = end - begin;
      if (fifo.size >= num) {
        // SIMD 8 4-bit lanes

        const uint32_t one8 = 0x11111111;
        const uint32_t three8 = 0x33333333;  
        
        uint32_t color8 = Fifo64Pop(&fifo, num) << begin;

        // Mask based on clip left and right.
        int clip_begin = Max(begin, clip_left - dest_x);
        int clip_end = Min(end, clip_right - dest_x);
        uint32_t mask8 = ((1 << (clip_end - clip_begin)) - 1) << clip_begin;
        
        // Mask based on src_color==0. Bit zero of each lane to mask value.
        uint32_t color_mask8 = color8;
        color_mask8 |= (color_mask8 & ~three8) >> 2;
        color_mask8 |= (color_mask8 & ~one8) >> 1;
        color_mask8 &= one8;

        // Mask value to all bits of each lane
        color_mask8 |= color_mask8 << 1;
        color_mask8 |= color_mask8 << 2;

        // Optionally, apply color mask.
        mask8 &= (color_mask8 | mask_disable8);

        // Optionally remap colors.
        color8 = (fg_color8 & color8) | (bg_color8 & ~color8);

        EndPerf(&g_blit_cycle_perf[opcode & BLIT_OP_SRC]);
        MCycle(2);
        WriteDestData(opcode, dest_daddr_word, dest_baddr, color8, mask8);

        dest_x += 32;
        ++dest_daddr_word;
        ++dest_baddr;
      } else {
        EndPerf(&g_blit_cycle_perf[opcode & BLIT_OP_SRC]);
        MCycle(2);
      }
    }

  }
}

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

    switch (opcode) {
    case OPCODE_SET0:
    case OPCODE_SET1:
    case OPCODE_SET2:
    case OPCODE_SET3:
    case OPCODE_SET4:
    case OPCODE_SET5:
    case OPCODE_SET6:
    case OPCODE_SET7:
      SetRegister(opcode, PopCmdFifo(16));
      break;
    case OPCODE_SWAP0:
    case OPCODE_SWAP1:
    case OPCODE_SWAP2:
    case OPCODE_SWAP3:
      DoSwap((SwapMode) (opcode & SWAP_MASK));
      break;
    case OPCODE_NOP:
      break;
    default:
      assert(opcode & OPCODE_BLIT_BASE);
      DoBlit(opcode);
      break;
    }
  }
}
