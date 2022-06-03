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
  BLIT_FLAG_UNZIP        = 0x0300,
  BLIT_FLAG_UNZIP_OFF    = 0x0000,
  BLIT_FLAG_UNZIP_2X     = 0x0100,
  BLIT_FLAG_UNZIP_4X     = 0x0200,
  BLIT_FLAG_MASKED       = 0x0400,
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

static DisplayBank* g_blit_display_bank;
static uint16_t g_blit_regs[NUM_BLIT_REGS];
static uint16_t g_last_mcycle;

uint32_t g_blit_bank[BLITTER_BANK_SIZE];

static int g_source_fifo_addr;
static int g_source_fifo_buf;
static uint32_t g_source_fifo_bits;

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

static uint32_t STRIPED_SECTION ReadBlitterBank(unsigned addr) {
  return g_blit_bank[addr & (BLITTER_BANK_SIZE-1)];
}

// Byte offset
static void STRIPED_SECTION WriteBlitterBank(unsigned addr, uint32_t data) {
  g_blit_bank[addr & (BLITTER_BANK_SIZE-1)] = data;
}

static uint32_t STRIPED_SECTION ReadDisplayBank(unsigned addr) {
  return g_blit_display_bank->words[addr & (DISPLAY_BANK_SIZE-1)];
}

static void STRIPED_SECTION WriteDisplayBank(unsigned addr, uint32_t data) {
  g_blit_display_bank->words[addr & (DISPLAY_BANK_SIZE-1)] = data;
}

static void STRIPED_SECTION InitSourceFifo(Opcode opcode) {
  g_source_fifo_bits = 0;

  switch (opcode & BLIT_OP_SRC) {
  case BLIT_OP_SRC_BLITTER:
  case BLIT_OP_SRC_DISPLAY:
    g_source_fifo_addr = g_blit_regs[BLIT_REG_SRC_ADDR];
    break;
  default:
    g_source_fifo_addr = 0;  // not used
    break;
  }
}

static uint32_t STRIPED_SECTION PushSourceFifo(int buf) {
  g_source_fifo_buf = buf;
  g_source_fifo_bits = 32;
}

static uint32_t STRIPED_SECTION PopSourceFifo(int n) {
    int data = g_source_fifo_buf & ((1 << n) - 1);
    g_source_fifo_buf >>= n;
    g_source_fifo_bits -= n;
    return data;
}

static uint32_t STRIPED_SECTION UnzipSourceFifo(Opcode opcode) {
  int flags = g_blit_regs[BLIT_REG_FLAGS];
  int data;

  if (!(opcode & BLIT_OP_FLAGS_EN)) {
    flags = BLIT_FLAG_UNZIP_OFF;
  }

  switch (flags & BLIT_FLAG_UNZIP) {
  case BLIT_FLAG_UNZIP_OFF:
    return PopSourceFifo(4);
  case BLIT_FLAG_UNZIP_2X:
    return PopSourceFifo(2);
  case BLIT_FLAG_UNZIP_4X:
    return PopSourceFifo(1);
  }

  return data;
}

// Returns next 4 bits
static int STRIPED_SECTION ReadSourceFifo(Opcode opcode) {
  switch (opcode & BLIT_OP_SRC) {
  case BLIT_OP_SRC_BLITTER:
    if (g_source_fifo_bits == 0) {
      PushSourceFifo(ReadBlitterBank(g_source_fifo_addr++));
    }
    return UnzipSourceFifo(opcode);

  case BLIT_OP_SRC_DISPLAY:
    if (g_source_fifo_bits == 0) {
      PushSourceFifo(ReadDisplayBank(g_source_fifo_addr++));
    }
    return PopSourceFifo(4);

  case BLIT_OP_SRC_ZERO:
    return 0;

  case BLIT_OP_SRC_STREAM:
    if (g_source_fifo_bits == 0) {
      PushSourceFifo(PopCmdFifo32());
    }
    return PopSourceFifo(4);
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
  int daddr_dest = g_blit_regs[BLIT_REG_DST_ADDR];
  int daddr_src = g_blit_regs[BLIT_REG_SRC_ADDR];
  int baddr_dest = g_blit_regs[BLIT_REG_DST_ADDR];
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

  InitSourceFifo(opcode);

  for (int y = 0; y < height; ++y) {
    int daddr_dest_word = daddr_dest >> 3;
    int daddr_source_word = daddr_src >> 3;
    int daddr_dest_nibble = daddr_dest & 0x7;
    int daddr_source_nibble = daddr_src & 0x7;

    int outer_width = (width + 7) / 8;
    int x = 0;
    uint64_t in_shift = 0;
    if ((opcode & BLIT_OP_TOPY) == BLIT_OP_TOPY_PLAN) {
      x = -daddr_dest_nibble;
      ++outer_width;
    }

    uint64_t in_data = 0;
    for (int ox = 0; ox < outer_width; ++ox) {
      MCycle();

      uint32_t old_out_data = ReadDestData(opcode, daddr_dest_word + ox, baddr_dest);
      uint32_t new_out_data = 0;
      for (int i = 0; i < 32; i += 4) {
        int old_color = (old_out_data >> i) & 0xF;
        int new_color = old_color;

        if (x >= 0 && x < width) {
          int src_color = ReadSourceFifo(opcode);
          
          if (x >= clip_left && x <= clip_right) {
            if (src_color | unmasked) {
              if (src_color < 4) {
                src_color = (cmap >> (src_color*4)) & 0xF;
              }
              
              new_color = src_color;
            }
          }
        }

        new_out_data |= new_color << i;
        ++x;
      }

      WriteDestData(opcode, daddr_dest_word + ox, baddr_dest, new_out_data);
      ++baddr_dest;
    }

    daddr_dest += pitch;
    daddr_src += pitch;
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
