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
  BLIT_FLAG_DEST_LOGIC   = 0x3000,
  BLIT_FLAG_DEST_AND     = 0x1000,
  BLIT_FLAG_DEST_OR      = 0x2000,
  BLIT_FLAG_DEST_XOR     = 0x3000,
};

enum {
  BLIT_REG_DADDR         = 0,  // 16-bit. Address of nibble in display bank.
  BLIT_REG_CLIP          = 1,  // 16-bit. Left side of unclipped area.
  BLIT_REG_LADDR         = 2,  // 16-bit. Address of a 32-bit word in local bank.
  BLIT_REG_COUNT         = 3,  // 16-bit. Iteration count or count pair.
  BLIT_REG_DPITCH        = 4,  // 9-bit. Offset added to display address.
  BLIT_REG_FLAGS         = 5,  // 16-bit. Miscellaneous flags.
  BLIT_REG_DADDR2        = 6,  // 16-bit. Address of nibble in display bank.
  BLIT_REG_LADDR2        = 7,  // 16-bit. Address of a 32-bit word in local bank.
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

  OPCODE_SET01      = 0x10,
  OPCODE_SET23      = 0x12,
  OPCODE_SET45      = 0x14,
  OPCODE_SET67      = 0x16,

  OPCODE_MOVE       = 0x20,
  OPCODE_QMOVE      = 0x21,
  OPCODE_MOVE2      = 0x26,
  OPCODE_QMOVE2     = 0x27,
  
  OPCODE_DSTREAM    = 0x30,
  OPCODE_DRSTREAM   = 0x31,
  OPCODE_DDCOPY     = 0x32,
  OPCODE_LDCOPY     = 0x33,
  OPCODE_DCLEAR     = 0x34,

  OPCODE_LSTREAM    = 0x38,
  OPCODE_DLCOPY     = 0x39,
  OPCODE_LLCOPY     = 0x3A,

  OPCODE_RECT       = 0x80,
  OPCODE_BLIT       = 0x88,
  OPCODE_SAVE       = 0x90,
  OPCODE_RESTORE    = 0x91,

  OPCODE_SWAP0      = 0xF8,
  OPCODE_SWAP1      = 0xF9,
  OPCODE_SWAP2      = 0xFA,
  OPCODE_SWAP3      = 0xFB,
  OPCODE_NOP        = 0xFF,
} Opcode;

static DisplayBank* g_blit_bank;
static uint16_t g_blit_regs[NUM_BLIT_REGS];
static uint32_t g_local_bank[LOCAL_BANK_SIZE];
static uint16_t g_last_mcycle;

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

static uint32_t STRIPED_SECTION ApplyDrawLogic(uint32_t display_colors8, int src_color, int display_shift) {
  const int display_bits_per_pixel = 4;
  const int display_pixels_per_word = 32 / display_bits_per_pixel;
  const int display_pixel_mask = (1 << display_bits_per_pixel) - 1;
  
  int flags = g_blit_regs[BLIT_REG_FLAGS];
  int dest_logic = flags & BLIT_FLAG_DEST_LOGIC;

  if (src_color < 4) {
    src_color = (g_blit_regs[BLIT_REG_CMAP] >> (src_color*4)) & 0xF;
  }

  switch (dest_logic) {
    case BLIT_FLAG_DEST_AND:
      return display_colors8 & ~((~src_color) << display_shift);
    case BLIT_FLAG_DEST_OR:
      return display_colors8 | (src_color << display_shift);
    case BLIT_FLAG_DEST_XOR:
      return display_colors8 ^ (src_color << display_shift);
    default:
      return (display_colors8 & ~(display_pixel_mask << display_shift)) | (src_color << display_shift);
  }
}

static void STRIPED_SECTION DoStreamLocal() {
  int addr = g_blit_regs[BLIT_REG_LADDR];
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

  int addr = g_blit_regs[BLIT_REG_DADDR] >> 3;
  int count = g_blit_regs[BLIT_REG_COUNT];

  for (int i = 0; i < count; ++i) {
    MCycle();    
    WriteDisplayRAM(addr + i, data);
  }
}

static void STRIPED_SECTION DoStreamDisplay() {
  int addr = g_blit_regs[BLIT_REG_DADDR] >> 3;
  int count = g_blit_regs[BLIT_REG_COUNT];

  for (int i = 0; i < count; ++i) {
    MCycle();    
    uint32_t data = PopFifoBlocking32();
    WriteDisplayRAM(addr + i, data);
  }
}

static void STRIPED_SECTION DoStreamDisplayRect() {
  int addr = g_blit_regs[BLIT_REG_DADDR] >> 3;
  int pitch = (int16_t) g_blit_regs[BLIT_REG_DPITCH];

  int width, height;
  GetDimensions(&width, &height);

  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      MCycle();    
      uint32_t data = PopFifoBlocking32();
      WriteDisplayRAM(addr + x, data);
    }
    
    addr += pitch;
  }
}

static void STRIPED_SECTION Blit(int blit_addr) {
  const int display_bits_per_pixel = 4;
  const int display_pixels_per_word = 32 / display_bits_per_pixel;
  const int display_pixel_mask = (1 << display_bits_per_pixel) - 1;

  int display_addr = g_blit_regs[BLIT_REG_DADDR];
  int pitch = (int16_t) g_blit_regs[BLIT_REG_DPITCH];
  int counts = g_blit_regs[BLIT_REG_COUNT];
  int clip = g_blit_regs[BLIT_REG_CLIP];
  int flags = g_blit_regs[BLIT_REG_FLAGS];

  int clip_left = clip & 0xFF;
  int clip_right = clip >> 8;
  int blit_width = counts & 0xFF;
  int blit_height = counts >> 8;
  int dest_logic = flags & BLIT_FLAG_DEST_LOGIC;

  int unmasked = !(flags & BLIT_FLAG_MASKED);

  int blit_bits_per_pixel;
  switch (flags & BLIT_FLAG_UNZIP) {
  case BLIT_FLAG_UNZIP_4X:
    blit_bits_per_pixel = 1;
    break;
  case BLIT_FLAG_UNZIP_2X:
    blit_bits_per_pixel = 2;
    break;
  default:
    blit_bits_per_pixel = 4;
    break;
  }

  int blit_pixel_mask = (1 << blit_bits_per_pixel) - 1;

  uint32_t blit_colors = ReadLocalRAM(blit_addr++);
  int blit_shift = 0;

  for (int y = 0; y < blit_height; ++y) {
    int display_word_addr = display_addr / display_pixels_per_word;

    uint32_t display_colors8 = ReadDisplayRAM(display_word_addr);
    int display_shift = (display_addr & (display_pixels_per_word-1)) * display_bits_per_pixel;

    for (int x = 0; x < blit_width; ++x) {
      int color = (blit_colors >> blit_shift) & blit_pixel_mask;
      if (color | unmasked) {
        if (x >= clip_left && x <= clip_right) {
          display_colors8 = ApplyDrawLogic(display_colors8, color, display_shift);
        }
      }

      blit_shift += blit_bits_per_pixel;
      if (blit_shift == 32) {
        blit_colors = ReadLocalRAM(blit_addr++);
        blit_shift = 0;
      }

      display_shift += display_bits_per_pixel;
      if (display_shift == 32) {
        MCycle();
        if (dest_logic) {
          // Extra cycle for read-modify-write of display RAM rather just write.
          MCycle();
        }

        WriteDisplayRAM(display_word_addr++, display_colors8);
        display_colors8 = ReadDisplayRAM(display_word_addr);
        display_shift = 0;
      }
    }

    if (display_shift) {
      MCycle();
      if (dest_logic) {
        MCycle();
      }
        
      WriteDisplayRAM(display_word_addr, display_colors8);
    }

    display_addr += pitch;
  }
}

static void STRIPED_SECTION DoBlit() {
  Blit(g_blit_regs[BLIT_REG_LADDR]);
}

// TODO: should there be a version of this command that copies in reverse in case of overlap?
static void STRIPED_SECTION DoDisplayToDisplayCopy() {
  int dest_addr = g_blit_regs[BLIT_REG_DADDR] >> 3;
  int pitch = (int16_t) g_blit_regs[BLIT_REG_DPITCH];
  int source_addr = g_blit_regs[BLIT_REG_DADDR2] >> 3;

  int width, height;
  GetDimensions(&width, &height);

  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      MCycle();    
      uint32_t data = ReadDisplayRAM(source_addr + x);
      WriteLocalRAM(dest_addr + x, data);
    }
    
    source_addr += pitch;
    dest_addr += pitch;
  }
}

static void STRIPED_SECTION DoDisplayToLocalCopy() {
  int display_addr = g_blit_regs[BLIT_REG_DADDR] >> 3;
  int pitch = (int16_t) g_blit_regs[BLIT_REG_DPITCH];
  int local_addr = g_blit_regs[BLIT_REG_LADDR];

  int width, height;
  GetDimensions(&width, &height);

  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      MCycle();    
      uint32_t data = ReadDisplayRAM(display_addr + x);
      WriteLocalRAM(local_addr++, data);
    }
    
    display_addr += pitch;
  }
}

static void STRIPED_SECTION DoLocalToDisplayCopy() {
  int display_addr = g_blit_regs[BLIT_REG_DADDR] >> 3;
  int pitch = (int16_t) g_blit_regs[BLIT_REG_DPITCH];
  int local_addr = g_blit_regs[BLIT_REG_LADDR];

  int width, height;
  GetDimensions(&width, &height);

  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      MCycle();    
      uint32_t data = ReadLocalRAM(local_addr++);
      WriteDisplayRAM(display_addr + x, data);
    }
    
    display_addr += pitch;
  }
}

// TODO: should there be a version of this command that copies in reverse in case of overlap?
static void STRIPED_SECTION DoLocalToLocalCopy() {
  int dest_addr = g_blit_regs[BLIT_REG_LADDR];
  int source_addr = g_blit_regs[BLIT_REG_LADDR2];
  int size = g_blit_regs[BLIT_REG_COUNT];

  for (int i = 0; i < size; ++i) {
    MCycle();
    uint32_t data = ReadLocalRAM(source_addr++);
    WriteLocalRAM(dest_addr++, data);
  }
}

static void STRIPED_SECTION DoMove(int reg_idx, int operand) {
  int value = g_blit_regs[reg_idx];
  SetRegister(reg_idx, value + operand);
}

static void STRIPED_SECTION DoRect() {
  const int display_bits_per_pixel = 4;
  const int display_pixels_per_word = 32 / display_bits_per_pixel;
  const int display_pixel_mask = (1 << display_bits_per_pixel) - 1;
  
  int display_addr = g_blit_regs[BLIT_REG_DADDR];
  int pitch = g_blit_regs[BLIT_REG_DPITCH];
  int color = g_blit_regs[BLIT_REG_CMAP] & 0xF;

  int width, height;
  GetDimensions(&width, &height);
  
  for (int y = 0; y < height; ++y) {
    int display_word_addr = display_addr / display_pixels_per_word;

    uint32_t display_colors8 = ReadDisplayRAM(display_word_addr);
    int display_shift = (display_addr & (display_pixels_per_word-1)) * display_bits_per_pixel;

    for (int x = 0; x < width; ++x) {
      display_colors8 = ApplyDrawLogic(display_colors8, color, display_shift);
      display_shift += display_bits_per_pixel;
      if (display_shift == 32) {
        MCycle();
        WriteDisplayRAM(display_word_addr++, display_colors8);
        display_colors8 = ReadDisplayRAM(display_word_addr);
        display_shift = 0;
      }
    }

    if (display_shift) {
      MCycle();
      WriteDisplayRAM(display_word_addr, display_colors8);
    }

    display_addr += pitch;
  }
}

static void STRIPED_SECTION DoRestore() {
  int save_addr = g_blit_regs[BLIT_REG_LADDR2];
  if (save_addr == 0) {
    save_addr = 0x10000;
  }

  const int end_addr = 0x10000;

  while (save_addr < end_addr) {
    MCycle();
    uint32_t header = ReadLocalRAM(save_addr++);
    int display_addr = header & 0xFFFF;
    int w = header >> 16;

    if (save_addr >= end_addr) {
      break;
    }

    for (int x = 0; x < w; ++x) {
      MCycle();

      uint32_t data = ReadLocalRAM(save_addr++);
      WriteDisplayRAM(display_addr + x, data);

      if (save_addr >= end_addr) {
        break;
      }
    }
  }

  SetRegister(BLIT_REG_LADDR2, save_addr);
}

static void STRIPED_SECTION DoSave() {
  int display_addr = g_blit_regs[BLIT_REG_DADDR] >> 3;
  int pitch = g_blit_regs[BLIT_REG_DPITCH];
  int save_addr = g_blit_regs[BLIT_REG_LADDR2];

  int width, height;
  GetDimensions(&width, &height);

  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      MCycle();    
      uint32_t data = ReadDisplayRAM(display_addr + x);
      WriteLocalRAM(--save_addr, data);
    }
    
    MCycle();
    WriteLocalRAM(--save_addr, display_addr | (width << 16));

    display_addr += pitch;
  }

  SetRegister(BLIT_REG_LADDR2, save_addr);
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
      DoBlit();
      break;
    case OPCODE_DDCOPY:
      DoDisplayToDisplayCopy();
      break;
    case OPCODE_DLCOPY:
      DoDisplayToLocalCopy();
      break;
    case OPCODE_DCLEAR:
      DoClearDisplay(PopFifoBlocking8());
      break;
    case OPCODE_DSTREAM:
      DoStreamDisplay();
      break;
    case OPCODE_DRSTREAM:
      DoStreamDisplayRect();
      break;
    case OPCODE_LDCOPY:
      DoLocalToDisplayCopy();
      break;
    case OPCODE_LLCOPY:
      DoLocalToLocalCopy();
      break;
    case OPCODE_LSTREAM:
      DoStreamLocal();
      break;
    case OPCODE_MOVE:
      DoMove(BLIT_REG_DADDR, PopFifoBlocking16());
      break;
    case OPCODE_MOVE2:
      DoMove(BLIT_REG_DADDR2, PopFifoBlocking16());
      break;
    case OPCODE_QMOVE:
      DoMove(BLIT_REG_DADDR, PopFifoBlocking8());
      break;
    case OPCODE_QMOVE2:
      DoMove(BLIT_REG_DADDR2, PopFifoBlocking8());
      break;
    case OPCODE_RECT:
      DoRect();
      break;
    case OPCODE_RESTORE:
      DoRestore();
      break;
    case OPCODE_SAVE:
      DoSave();
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
    case OPCODE_SET01:
      SetRegister(0, PopFifoBlocking16());
      SetRegister(1, PopFifoBlocking16());
      break;
    case OPCODE_SET23:
      SetRegister(2, PopFifoBlocking16());
      SetRegister(3, PopFifoBlocking16());
      break;
    case OPCODE_SET45:
      SetRegister(4, PopFifoBlocking16());
      SetRegister(5, PopFifoBlocking16());
      break;
    case OPCODE_SET67:
      SetRegister(6, PopFifoBlocking16());
      SetRegister(7, PopFifoBlocking16());
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
