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

#define NUM_BLIT_REGS 16
#define LOCAL_BANK_SIZE (128 * 1024 / sizeof(uint32_t))
#define MCYCLE_TIME 16

enum {
  BLIT_FLAG_UNZIP        = 0x0300,
  BLIT_FLAG_UNZIP_OFF    = 0x0000,
  BLIT_FLAG_UNZIP_2X     = 0x0100,
  BLIT_FLAG_UNZIP_4X     = 0x0200,
  BLIT_FLAG_MASKED       = 0x0400,
};

enum {
  BLIT_REG_DADDR_DST     = 0,  // 16-bit. Address of nibble in display bank.
  BLIT_REG_CLIP          = 1,  // 16-bit. Left side of unclipped area.
  BLIT_REG_LADDR_SRC     = 2,  // 16-bit. Address of a 32-bit word in local bank.
  BLIT_REG_COUNT         = 3,  // 16-bit. Iteration count or count pair.
  BLIT_REG_DPITCH        = 4,  // 9-bit. Offset added to display address.
  BLIT_REG_FLAGS         = 5,  // 16-bit. Miscellaneous flags.
  BLIT_REG_DADDR_SRC     = 6,  // 16-bit. Address of nibble in display bank.
  BLIT_REG_LADDR_DST     = 7,  // 16-bit. Address of a 32-bit word in local bank.
  BLIT_REG_CMAP          = 8,  // 16-bit. Array of colors colors 0-3 are remapped to.
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

  OPCODE_DSTREAM    = 0x30,//
  OPCODE_DCLEAR     = 0x34,

  OPCODE_LSTREAM    = 0x38,//

  OPCODE_RECT       = 0x80,//
  OPCODE_BLIT       = 0x88,//

  OPCODE_SWAP0      = 0xF8,
  OPCODE_SWAP1      = 0xF9,
  OPCODE_SWAP2      = 0xFA,
  OPCODE_SWAP3      = 0xFB,
  OPCODE_NOP        = 0xFF,
} Opcode;

static DisplayBank* g_blit_bank;
static uint16_t g_blit_regs[NUM_BLIT_REGS];
static uint16_t g_last_mcycle;

uint32_t g_local_bank[LOCAL_BANK_SIZE];

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

static int STRIPED_SECTION PopFifoBlocking8() {
  while (IsFifoEmpty()) {
    MCycle();
  }
  return PopFifo();
}

static int STRIPED_SECTION PopFifoBlocking16() {
  int low = PopFifoBlocking8();
  int high = PopFifoBlocking8();
  return low | (high << 8);
}

static uint32_t STRIPED_SECTION PopFifoBlocking32() {
  int low = PopFifoBlocking16();
  int high = PopFifoBlocking16();
  return low | (high << 16);
}

static int STRIPED_SECTION GetRegister(int idx) {
  return g_blit_regs[idx & (NUM_BLIT_REGS-1)];
}

static void STRIPED_SECTION SetRegister(int idx, int data) {
  g_blit_regs[idx & (NUM_BLIT_REGS-1)] = data;
}

static uint32_t STRIPED_SECTION ReadLocalRAM(unsigned addr) {
  return g_local_bank[addr & (LOCAL_BANK_SIZE-1)];
}

// Byte offset
static void STRIPED_SECTION WriteLocalRAM(unsigned addr, uint32_t data) {
  g_local_bank[addr & (LOCAL_BANK_SIZE-1)] = data;
}

static uint32_t STRIPED_SECTION ReadDisplayRAM(unsigned addr) {
  return g_blit_bank->words[addr & (DISPLAY_BANK_SIZE-1)];
}

static void STRIPED_SECTION WriteDisplayRAM(unsigned addr, uint32_t data) {
  g_blit_bank->words[addr & (DISPLAY_BANK_SIZE-1)] = data;
}

static void STRIPED_SECTION GetDimensions(int* width, int* height) {
  int count = g_blit_regs[BLIT_REG_COUNT];
  *width = g_blit_regs[BLIT_REG_COUNT] & 0xFF;
  if (*width == 0) {
    *width = 0x100;
  }
  *height = g_blit_regs[BLIT_REG_COUNT] >> 8;
  if (*height == 0) {
    *height = 0x100;
  }
}

static void STRIPED_SECTION DoStreamLocal() {
  int addr = g_blit_regs[BLIT_REG_LADDR_DST];
  int size = g_blit_regs[BLIT_REG_COUNT];

  for (int i = 0; i < size; ++i) {
    MCycle();
    uint32_t data = PopFifoBlocking32();
    WriteLocalRAM(addr, data);
    ++addr;
  }
}

static void STRIPED_SECTION DoClearDisplay(int data) {
  data = (data << 8) | data;
  data = (data << 16) | data;

  int addr = g_blit_regs[BLIT_REG_DADDR_DST] >> 3;
  int count = g_blit_regs[BLIT_REG_COUNT];

  for (int i = 0; i < count; ++i) {
    MCycle();    
    WriteDisplayRAM(addr + i, data);
  }
}

static void STRIPED_SECTION DoStreamDisplay() {
  int addr = g_blit_regs[BLIT_REG_DADDR_DST] >> 3;
  int count = g_blit_regs[BLIT_REG_COUNT];

  for (int i = 0; i < count; ++i) {
    MCycle();    
    uint32_t data = PopFifoBlocking32();
    WriteDisplayRAM(addr + i, data);
  }
}

static void STRIPED_SECTION InitSourceFifo(Opcode opcode) {
  g_source_fifo_bits = 0;

  switch (opcode) {
  case OPCODE_BLIT:
    g_source_fifo_addr = g_blit_regs[BLIT_REG_LADDR_SRC];
    break;
  case OPCODE_RECT:
    g_source_fifo_addr = 0;  // not used
    break;
  }
}

static uint32_t STRIPED_SECTION UnzipSourceFifo() {
  int flags = g_blit_regs[BLIT_REG_FLAGS];
  int data;

  switch (flags & BLIT_FLAG_UNZIP) {
  case BLIT_FLAG_UNZIP_OFF:
    data = g_source_fifo_buf & 0xF;
    g_source_fifo_buf >>= 4;
    g_source_fifo_bits -= 4;
    break;
  case BLIT_FLAG_UNZIP_2X:
    data = g_source_fifo_buf & 0x3;
    g_source_fifo_buf >>= 2;
    g_source_fifo_bits -= 2;
    break;
  case BLIT_FLAG_UNZIP_4X:
    data = g_source_fifo_buf & 0x1;
    g_source_fifo_buf >>= 1;
    g_source_fifo_bits -= 1;
    break;
  }

  return data;
}

// Returns next 4 bits
static int STRIPED_SECTION ReadSourceFifo(Opcode opcode) {
  switch (opcode) {
    case OPCODE_BLIT: {
      if (g_source_fifo_bits == 0) {
        g_source_fifo_buf = ReadLocalRAM(g_source_fifo_addr++);
        g_source_fifo_bits = 32;
      }

      return UnzipSourceFifo();
    }
    case OPCODE_RECT: {
      return 0;
    }
  }
}

static uint32_t STRIPED_SECTION ReadDestData(Opcode opcode, int daddr, int laddr) {
  switch (opcode) {
  case OPCODE_BLIT:
  case OPCODE_RECT:
    return ReadDisplayRAM(daddr);
  }
}

static uint32_t STRIPED_SECTION WriteDestData(Opcode opcode, int daddr, int laddr, uint32_t data) {
  switch (opcode) {
  case OPCODE_BLIT:
  case OPCODE_RECT:
    WriteDisplayRAM(daddr, data);
    break;
  }
}

static void STRIPED_SECTION DoBlit(Opcode opcode) {
  int daddr_dest = g_blit_regs[BLIT_REG_DADDR_DST];
  int daddr_src = g_blit_regs[BLIT_REG_DADDR_SRC];
  int laddr_dest = g_blit_regs[BLIT_REG_LADDR_DST];
  int laddr_src = g_blit_regs[BLIT_REG_LADDR_SRC];
  int pitch = (int16_t) g_blit_regs[BLIT_REG_DPITCH];
  int flags = g_blit_regs[BLIT_REG_FLAGS];

  int width, height;
  GetDimensions(&width, &height);

  // Except for particular opcodes, these fields default to nop values.
  int unmasked = 1;
  int clip_left = 0;
  int clip_right = 0xFF;
  int cmap = 0x3210;
  if (opcode == OPCODE_BLIT) {
    unmasked = !(flags & BLIT_FLAG_MASKED);
    
    int clip = g_blit_regs[BLIT_REG_CLIP];
    clip_left = clip & 0xFF;
    clip_right = clip >> 8;
  }
  if (opcode == OPCODE_BLIT || opcode == OPCODE_RECT) {
    cmap = g_blit_regs[BLIT_REG_CMAP];
  }

  int outer_width = (width + 15) / 8;

  InitSourceFifo(opcode);

  for (int y = 0; y < height; ++y) {
    int daddr_dest_word = daddr_dest >> 3;
    int daddr_source_word = daddr_src >> 3;
    int daddr_dest_nibble = daddr_dest & 0x7;
    int daddr_source_nibble = daddr_src & 0x7;

    int x = 0;
    uint64_t in_shift = 0;
    switch (opcode) {
    case OPCODE_BLIT:
    case OPCODE_RECT:
      x = -daddr_dest_nibble;
      break;
    }

    uint64_t in_data = 0;
    for (int ox = 0; ox < outer_width; ++ox) {
      MCycle();

      uint32_t old_out_data = ReadDestData(opcode, daddr_dest_word + ox, laddr_dest);
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

      WriteDestData(opcode, daddr_dest_word + ox, laddr_dest, new_out_data);
      laddr_dest += 4;
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

  g_blit_bank = GetBlitBank();
}

void STRIPED_SECTION BlitMain() {
  g_blit_bank = GetBlitBank();

  for (;;) {
    int opcode = PopFifoBlocking8();
    MCycle();

    switch (opcode) {
    case OPCODE_BLIT:
    case OPCODE_RECT:
      DoBlit(opcode);
      break;
    case OPCODE_DCLEAR:
      DoClearDisplay(PopFifoBlocking8());
      break;
    case OPCODE_DSTREAM:
      DoStreamDisplay();
      break;
    case OPCODE_LSTREAM:
      DoStreamLocal();
      break;
    case OPCODE_SET0:
    case OPCODE_SET1:
    case OPCODE_SET2:
    case OPCODE_SET3:
    case OPCODE_SET4:
    case OPCODE_SET5:
    case OPCODE_SET6:
    case OPCODE_SET7:
    case OPCODE_SET8:
      SetRegister(opcode, PopFifoBlocking16());
      break;
    case OPCODE_SWAP0:
    case OPCODE_SWAP1:
    case OPCODE_SWAP2:
    case OPCODE_SWAP3:
      DoSwap((SwapMode) (opcode & SWAP_MASK), PopFifoBlocking8());
      break;
    default:
      break;
    }
  }
}
