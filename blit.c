#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "pico/stdlib.h"

#include "blit.h"

#include "pins.h"
#include "scan_out.h"
#include "section.h"
#include "sys80.h"
#include "video_dma.h"

#define NUM_BLIT_REGS 8
#define BLITTER_BANK_SIZE (128 * 1024 / sizeof(uint32_t))
#define MCYCLE_TIME 16

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
  BLIT_REG_CMAP           = 6,  // Array of colors colors 0-3 are remapped to.
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
  BLIT_OP_CMAP_EN         = 0x40,
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
 
  OPCODE_DCLEAR     = OPCODE_BLIT_BASE | BLIT_OP_SRC_ZERO    | BLIT_OP_DEST_DISPLAY | BLIT_OP_TOPY_LIN                                       | BLIT_OP_CMAP_EN,  // 0xCA
  OPCODE_DDCOPY     = OPCODE_BLIT_BASE | BLIT_OP_SRC_DISPLAY | BLIT_OP_DEST_DISPLAY | BLIT_OP_TOPY_PLAN |                    BLIT_OP_CLIP_EN,                    // 0xA0
  OPCODE_DSTREAM    = OPCODE_BLIT_BASE | BLIT_OP_SRC_STREAM  | BLIT_OP_DEST_DISPLAY | BLIT_OP_TOPY_LIN,                                                          // 0x8B
  OPCODE_BSTREAM    = OPCODE_BLIT_BASE | BLIT_OP_SRC_STREAM  | BLIT_OP_DEST_BLITTER | BLIT_OP_TOPY_LIN,                                                          // 0x8F
  OPCODE_RECT       = OPCODE_BLIT_BASE | BLIT_OP_SRC_ZERO    | BLIT_OP_DEST_DISPLAY | BLIT_OP_TOPY_PLAN |                                      BLIT_OP_CMAP_EN,  // 0xC2
  OPCODE_IMAGE      = OPCODE_BLIT_BASE | BLIT_OP_SRC_BLITTER | BLIT_OP_DEST_DISPLAY | BLIT_OP_TOPY_PLAN | BLIT_OP_FLAGS_EN | BLIT_OP_CLIP_EN | BLIT_OP_CMAP_EN,  // 0xF1

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
static uint16_t g_last_mcycle;

union {
  uint8_t words8[BLITTER_BANK_SIZE*4];
  uint16_t words16[BLITTER_BANK_SIZE*2];
  uint32_t words32[BLITTER_BANK_SIZE];
} g_blit_bank;

static STRIPED_SECTION int Max(int a, int b) {
  return a > b ? a : b;
}

static STRIPED_SECTION int Min(int a, int b) {
  return a < b ? a : b;
}

static STRIPED_SECTION void Fifo64Push(Fifo64* fifo, int bits, int n) {
  fifo->bits |= ((uint64_t) bits) << ((uint64_t) fifo->size);
  fifo->size += n;
  assert(fifo->size <= 64);
}

static STRIPED_SECTION int Fifo64Pop(Fifo64* fifo, int n) {
  int bits = fifo->bits & ((1 << n) - 1);
  fifo->bits >>= n;
  fifo->size -= n;
  assert(fifo->size >= 0);
  return bits;
}

static void STRIPED_SECTION MCycle() {
  int dot_x, mcycle;
  do {
    do {
      dot_x = GetDotX();
      mcycle = dot_x / MCYCLE_TIME;
    } while (mcycle == g_last_mcycle);

    g_last_mcycle = mcycle;

  } while (!IsBlitClockEnabled(dot_x));
}

static int STRIPED_SECTION PopCmdFifo8() {
  while (IsFifoEmpty()) {
    MCycle();
  }
  return PopFifo();
}

static int STRIPED_SECTION PopCmdFifo16() {
  int low = PopCmdFifo8();
  int high = PopCmdFifo8();
  return low | (high << 8);
}

static uint32_t STRIPED_SECTION PopCmdFifo32() {
  int low = PopCmdFifo16();
  int high = PopCmdFifo16();
  return low | (high << 16);
}

static int STRIPED_SECTION GetRegister(int idx) {
  return g_blit_regs[idx & (NUM_BLIT_REGS-1)];
}

static void STRIPED_SECTION SetRegister(int idx, int data) {
  g_blit_regs[idx & (NUM_BLIT_REGS-1)] = data;
}

// 32-bit word offset as addr.
static uint32_t STRIPED_SECTION ReadBlitterBank(unsigned addr) {
  return g_blit_bank.words32[addr & (BLITTER_BANK_SIZE-1)];
}

static void STRIPED_SECTION WriteBlitterBank(unsigned addr, uint32_t data) {
  g_blit_bank.words32[addr & (BLITTER_BANK_SIZE-1)] = data;
}

// Zipped variants all use byte offsets as addr.
static int STRIPED_SECTION ReadZippedBlitterBank8(unsigned addr) {
  return g_blit_bank.words8[addr & (BLITTER_BANK_SIZE*4-1)];
}

static int STRIPED_SECTION ReadZippedBlitterBank16(unsigned addr) {
  return g_blit_bank.words16[(addr/2) & (BLITTER_BANK_SIZE*2-1)];
}

static uint32_t STRIPED_SECTION ReadZippedBlitterBank32(unsigned addr) {
  return g_blit_bank.words32[(addr/4) & (BLITTER_BANK_SIZE-1)];
}

static uint32_t STRIPED_SECTION ReadDisplayBank(unsigned addr) {
  return g_blit_display_bank->words[addr & (DISPLAY_BANK_SIZE-1)];
}

static void STRIPED_SECTION WriteDisplayBank(unsigned addr, uint32_t data) {
  g_blit_display_bank->words[addr & (DISPLAY_BANK_SIZE-1)] = data;
}

static uint32_t STRIPED_SECTION UnzipSourceData(Opcode opcode, int* baddr_byte) {
  int flags = g_blit_regs[BLIT_REG_FLAGS];
  if (!(opcode & BLIT_OP_FLAGS_EN)) {
    flags = BLIT_FLAG_UNPACK_OFF;
  }

  int unzipped = 0;
  switch (flags & BLIT_FLAG_UNPACK) {
    case BLIT_FLAG_UNPACK_OFF: {
      uint32_t zipped = ReadZippedBlitterBank32(*baddr_byte);
      *baddr_byte += 4;
      return zipped;
    }
    case BLIT_FLAG_UNPACK_16_32: {
      int zipped = ReadZippedBlitterBank16(*baddr_byte);
      int unzipped = 0;
      for (int i = 0; i < 8; ++i) {
        unzipped |= (((zipped) >> (i*2)) & 0x3) << (i*4);
      }
      *baddr_byte += 2;
      return unzipped;
    }
    case BLIT_FLAG_UNPACK_8_32: {
      int zipped = ReadZippedBlitterBank8(*baddr_byte);
      int unzipped = 0;
      for (int i = 0; i < 8; ++i) {
        unzipped |= (((zipped) >> i) & 0x1) << (i*4);
      }
      *baddr_byte += 1;
      return unzipped;
    }
    case BLIT_FLAG_UNPACK_8_16: {
      int zipped = ReadZippedBlitterBank8(*baddr_byte);
      int unzipped = 0;
      for (int i = 0; i < 4; ++i) {
        unzipped |= (((zipped) >> (i*2)) & 0x3) << (i*4);
      }
      *baddr_byte += 1;
      return unzipped | (unzipped << 16);
    }
  }
}

static uint32_t STRIPED_SECTION ReadSourceData(Opcode opcode, int daddr, int* baddr_byte) {
  switch (opcode & BLIT_OP_SRC) {
  case BLIT_OP_SRC_BLITTER:
    return UnzipSourceData(opcode, baddr_byte);
  case BLIT_OP_SRC_DISPLAY:
    return ReadDisplayBank(daddr);
  case BLIT_OP_SRC_ZERO:
    return 0;
  case BLIT_OP_SRC_STREAM:
    return PopCmdFifo32();
  }
}

static uint32_t STRIPED_SECTION ReadDestData(Opcode opcode, int daddr, int baddr) {
  switch (opcode & BLIT_OP_DEST) {
  case BLIT_OP_DEST_BLITTER:
    return ReadBlitterBank(baddr);
  case BLIT_OP_DEST_DISPLAY:
    return ReadDisplayBank(daddr);
  }
}

static uint32_t STRIPED_SECTION WriteDestData(Opcode opcode, int daddr, int baddr, uint32_t data) {
  switch (opcode & BLIT_OP_DEST) {
  case BLIT_OP_DEST_BLITTER:
    WriteBlitterBank(baddr, data);
  case BLIT_OP_DEST_DISPLAY:
    WriteDisplayBank(daddr, data);
  }
}

static void STRIPED_SECTION DoBlit(Opcode opcode) {
  int dest_daddr = g_blit_regs[BLIT_REG_DST_ADDR];
  int src_daddr = g_blit_regs[BLIT_REG_SRC_ADDR];
  int dest_baddr = g_blit_regs[BLIT_REG_DST_ADDR];
  int src_baddr_byte = g_blit_regs[BLIT_REG_SRC_ADDR] * 4;
  int flags = g_blit_regs[BLIT_REG_FLAGS];
  int pitch = (int16_t) g_blit_regs[BLIT_REG_PITCH];

  // Except for particular opcodes, these fields default to nop values.
  int unmasked = 1;
  if (opcode & BLIT_OP_FLAGS_EN) {
    unmasked = !(flags & BLIT_FLAG_MASKED);
  }

  int clip_left = 0;
  int clip_right = 0xFFFF;
  if (opcode & BLIT_OP_CLIP_EN) {
    int clip = g_blit_regs[BLIT_REG_CLIP];
    clip_left = clip & 0xFF;
    clip_right = clip >> 8;
  }

  int cmap = 0x3210;
  if (opcode & BLIT_OP_CMAP_EN) {
    cmap = g_blit_regs[BLIT_REG_CMAP];
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

  Fifo64 fifo = {0};

  int src_line_daddr = src_daddr - pitch;
  int src_daddr_word = 0;
  int src_x = width;
  int src_y = -1;
  int dest_line_daddr = dest_daddr - pitch;
  int dest_daddr_word = 0;
  int dest_x = width;
  int dest_y = -1;
  for (;;) {
    MCycle();

    if (dest_x >= width) {
      dest_line_daddr += pitch;
      dest_daddr_word = dest_line_daddr >> 3;

      ++dest_y;
      if (dest_y == height) {
        return;
      }

      if (dest_planar) {
        dest_x = -(dest_line_daddr & 0x7);
      } else {
        dest_x = 0;
      }
    }

    {
      Fifo64 save_fifo = fifo;
      uint32_t old_out_data = ReadDestData(opcode, dest_daddr_word, dest_baddr);
      uint32_t new_out_data = 0;
      for (int i = 0; i < 8; ++i) {
        int old_color = (old_out_data >> (i*4)) & 0xF;
        int new_color = old_color;

        if (dest_x+i >= 0 && dest_x+i < width) {
          if (fifo.size < 4) {
            fifo = save_fifo;
            goto skip_write;
          }
          int src_color = Fifo64Pop(&fifo, 4);
          
          if (dest_x+i >= clip_left && dest_x+i <= clip_right) {
            if (src_color | unmasked) {
              if (src_color < 4) {
                src_color = (cmap >> (src_color*4)) & 0xF;
              }
              
              new_color = src_color;
            }
          }
        }

        new_out_data |= new_color << (i*4);
      }

      WriteDestData(opcode, dest_daddr_word, dest_baddr, new_out_data);

      dest_x += 8;
      ++dest_daddr_word;
      ++dest_baddr;
    }

skip_write:
    
    if (src_x >= width) {
      src_line_daddr += pitch;
      src_daddr_word = src_line_daddr >> 3;

      ++src_y;

      if (src_planar) {
        src_x = -(src_line_daddr & 0x7);
      } else {
        src_x = 0;
      }
    }

    if (src_y < height && fifo.size <= 32) {
      uint32_t in_data = ReadSourceData(opcode, src_daddr_word, &src_baddr_byte);
      int begin = Max(0, -src_x*4);
      int end = Min(32, (width-src_x)*4);
      int num = end - begin;
      Fifo64Push(&fifo, (in_data >> begin) & ((1 << num) - 1), end - begin);
      /*for (int i = 0; i < 8; ++i) {
        if (src_x+i >= 0 && src_x+i < width) {
          Fifo64Push(&fifo, (in_data >> (i*4)) & 0xF, 4);
        }
      }*/
      src_x += 8;
      ++src_daddr_word;
    }
  }
}

static void STRIPED_SECTION DoSwap(SwapMode mode, int line_idx) {
  SwapBanks(mode, line_idx);

  while (IsSwapPending()) {
    MCycle();
  }

  g_blit_display_bank = GetBlitBank();
}

void STRIPED_SECTION BlitMain() {
  g_blit_display_bank = GetBlitBank();

  for (;;) {
    Opcode opcode = (Opcode) PopCmdFifo8();
    MCycle();

    switch (opcode) {
    case OPCODE_SET0:
    case OPCODE_SET1:
    case OPCODE_SET2:
    case OPCODE_SET3:
    case OPCODE_SET4:
    case OPCODE_SET5:
    case OPCODE_SET6:
    case OPCODE_SET7:
      SetRegister(opcode, PopCmdFifo16());
      break;
    case OPCODE_SWAP0:
    case OPCODE_SWAP1:
    case OPCODE_SWAP2:
    case OPCODE_SWAP3:
      DoSwap((SwapMode) (opcode & SWAP_MASK), PopCmdFifo8());
      break;
    default:
      assert(opcode & OPCODE_BLIT_BASE);
      DoBlit(opcode);
      break;
    }
  }
}
