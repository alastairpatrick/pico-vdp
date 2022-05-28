#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"

#include "section.h"
#include "sys80.h"
#include "video_dma.h"

#include "scan_out.h"

#define AWFUL_MAGENTA 0xC7

#define DISPLAY_PAGE_MASK 0xF
#define DISPLAY_PAGE_SIZE 512  // 32-bit words

static DisplayBank g_display_bank_a;
static DisplayBank g_display_bank_b;
static int g_scan_bank_idx;
static DisplayBank* g_scan_bank = &g_display_bank_a;
static DisplayBank* volatile g_blit_bank = &g_display_bank_a;
static volatile bool g_swap_pending;
static volatile SwapMode g_swap_mode;
static volatile int g_start_line_idx;

volatile bool g_display_blit_clock_enabled;
static bool g_lines_enabled;

static int g_pixels_addr;
static DisplayMode g_display_mode;

static int g_x_shift;
static int g_sprite_cycle;

static uint8_t SCAN_OUT_DATA_SECTION g_palette[16] = {
  0b00000000,
  0b11111111,
  0b00000001,
  0b00000010,
  0b00000100,
  0b00001000,
  0b00010000,
  0b00100000,

  0b01000000,
  0b10000000,
  0b11000000,
  0b00000001,
  0b00000010,
  0b00000011,
  0b00000100,
  0b00000110,
};

bool STRIPED_SECTION IsBlitClockEnabled(int dot_x) {
  if (dot_x < g_horz_blank_width) {
    return true;
  } else {
    return g_display_blit_clock_enabled;
  }
}

void STRIPED_SECTION SwapBanks(SwapMode mode, int line_idx) {
  g_swap_mode = mode;
  g_start_line_idx = line_idx;
  g_swap_pending = true;
}

bool STRIPED_SECTION IsSwapPending() {
  return g_swap_pending;
}

DisplayBank* STRIPED_SECTION GetBlitBank() {
  return g_blit_bank;
}

void STRIPED_SECTION Scroll(int line_idx) {
  g_start_line_idx = line_idx;
}

#pragma GCC push_options
#pragma GCC optimize("O3")

void SCAN_OUT_INNER_SECTION ScanOutSolid(uint8_t* dest, int width, uint8_t rgb) {
  for (int x = 0; x < width; ++x) {
    *dest++ = rgb;
  }
}

static uint32_t SCAN_OUT_INNER_SECTION ReadPixelData() {
  uint32_t data = g_scan_bank->words[g_pixels_addr & (DISPLAY_BANK_SIZE-1)];
  ++g_pixels_addr;
  return data;
}

static void SCAN_OUT_INNER_SECTION ScanOutLores2(uint8_t* dest, int width) {
  for (int x = 0; x < width/64; ++x) {
    uint32_t indices32 = ReadPixelData();

    for (int i = 0; i < 32; ++i) {
      *dest++ = *dest++ = g_palette[indices32 & 0x1];
      indices32 >>= 1;
    }
  }
}

static void SCAN_OUT_INNER_SECTION ScanOutHires2(uint8_t* dest, int width) {
  for (int x = 0; x < width/32; ++x) {
    uint32_t indices32 = ReadPixelData();

    #pragma GCC unroll 4
    for (int i = 0; i < 32; ++i) {
      *dest++ = g_palette[indices32 & 0x1];
      indices32 >>= 1;
    }
  }
}

static void SCAN_OUT_INNER_SECTION ScanOutLores4(uint8_t* dest, int width) {
  for (int x = 0; x < width/32; ++x) {
    uint32_t indices16 = ReadPixelData();

    for (int i = 0; i < 16; ++i) {
      *dest++ = *dest++ = g_palette[indices16 & 0x3];
      indices16 >>= 2;
    }
  }
}

static void SCAN_OUT_INNER_SECTION ScanOutHires4(uint8_t* dest, int width) {
  for (int x = 0; x < width/16; ++x) {
    uint32_t indices16 = ReadPixelData();

    for (int i = 0; i < 16; ++i) {
      *dest++ = g_palette[indices16 & 0x3];
      indices16 >>= 2;
    }
  }
}

static void SCAN_OUT_INNER_SECTION ScanOutLores16(uint8_t* dest, int width) {
  for (int x = 0; x < width/16; ++x) {
    uint32_t indices8 = ReadPixelData();

    for (int i = 0; i < 8; ++i) {
      *dest++ = *dest++ = g_palette[indices8 & 0xF];
      indices8 >>= 4;
    }
  }
}

static void SCAN_OUT_INNER_SECTION ScanOutHires16(uint8_t* dest, int width) {
  for (int x = 0; x < width/16; ++x) {
    uint32_t indices8_a = g_display_bank_a.words[g_pixels_addr & (DISPLAY_BANK_SIZE-1)];
    uint32_t indices8_b = g_display_bank_b.words[g_pixels_addr & (DISPLAY_BANK_SIZE-1)];
    ++g_pixels_addr;

    for (int i = 0; i < 8; ++i) {
      *dest++ = g_palette[indices8_a & 0xF];
      indices8_a >>= 4;

      *dest++ = g_palette[indices8_b & 0xF];
      indices8_b >>= 4;
    }
  }
}

static void SCAN_OUT_INNER_SECTION ScanOutLores256(uint8_t* dest, int width) {
  for (int x = 0; x < width/8; ++x) {
    uint32_t colors4_a = g_display_bank_a.words[g_pixels_addr & (DISPLAY_BANK_SIZE-1)];
    uint32_t colors4_b = g_display_bank_b.words[g_pixels_addr & (DISPLAY_BANK_SIZE-1)];
    ++g_pixels_addr;

    for (int i = 0; i < 4; ++i) {
      *dest++ = colors4_a;
      colors4_a >>= 8;

      *dest++ = colors4_b;
      colors4_b >>= 8;
    }
  }
}

static void SCAN_OUT_INNER_SECTION ScanOutLoresText(uint8_t* dest, int width, int y) {
  int font_base = (g_sys80_regs.font_page * DISPLAY_PAGE_SIZE * sizeof(uint32_t)) + (y & 0x7);

  for (int x = 0; x < width/16; ++x) {
    uint32_t char_word = ReadPixelData();

    char c = char_word & 0xFF;
    uint8_t palette[2] = {
      g_palette[(char_word >> 12) & 0xF],
      g_palette[(char_word >> 8) & 0xF],
    };
    uint attrs = (char_word >> 16) & 0xFF;

    uint8_t bits8 = g_scan_bank->bytes[font_base + c * 8];

    #pragma GCC unroll 4
    for (int j = 0; j < 8; ++j) {
      *dest++ = *dest++ = palette[bits8 & 0x1];
      bits8 >>= 1;
    }
  }
}

static void SCAN_OUT_INNER_SECTION ScanOutHiresText(uint8_t* dest, int width, int y) {
  int font_base = (g_sys80_regs.font_page * DISPLAY_PAGE_SIZE * sizeof(uint32_t)) + (y & 0x7);

  for (int x = 0; x < width/8; ++x) {
    uint32_t char_word = ReadPixelData();

    char c = char_word & 0xFF;
    uint8_t palette[2] = {
      g_palette[(char_word >> 12) & 0xF],
      g_palette[(char_word >> 8) & 0xF],
    };
    uint attrs = (char_word >> 16) & 0xFF;

    uint8_t bits8 = g_scan_bank->bytes[font_base + c * 8];

    #pragma GCC unroll 4
    for (int j = 0; j < 8; ++j) {
      *dest++ = palette[bits8 & 0x1];
      bits8 >>= 1;
    }
  }
}

void ScanOutSprite(uint8_t* dest, int width, int y) {
  int sprite_x = g_sys80_regs.sprite_x * 2;
  int sprite_rgb = g_sys80_regs.sprite_rgb;

  int sprite_bits = g_sys80_regs.sprite_bitmap[y];

  #pragma GCC unroll 4
  for (int x = 0; x < 16; ++x) {
    if ((sprite_bits >> x) & 1) {
      dest[sprite_x + x] = sprite_rgb;
    }
  }
}

#pragma GCC pop_options

void STRIPED_SECTION ScanOutBeginDisplay() {
  if (g_swap_pending) {
    switch (g_swap_mode) {
    case SWAP_SCAN_A_BLIT_A:
      g_blit_bank = g_scan_bank = &g_display_bank_a;
      break;
    case SWAP_SCAN_B_BLIT_B:
      g_blit_bank = g_scan_bank = &g_display_bank_b;
      break;
    case SWAP_SCAN_A_BLIT_B:
      g_scan_bank = &g_display_bank_a;
      g_blit_bank = &g_display_bank_b;
      break;
    case SWAP_SCAN_B_BLIT_A:
      g_scan_bank = &g_display_bank_b;
      g_blit_bank = &g_display_bank_a;
      break;
    }

    g_swap_pending = false;
  }

  if (++g_sprite_cycle >= g_sys80_regs.sprite_period) {
    g_sprite_cycle = 0;
  }

  g_lines_enabled = true;
}

void STRIPED_SECTION ScanOutLine(uint8_t* dest, int y, int width) {
  if (g_lines_enabled) {
    const int max_lines = 256;
    int lines_high = (g_sys80_regs.lines_page & DISPLAY_PAGE_MASK) * DISPLAY_PAGE_SIZE;
    int lines_low = ((g_start_line_idx + y) & 0xFF) * 2;
    ScanLine* line = g_scan_bank->lines + (lines_high | lines_low) / 2;

    if (line->display_mode_en) {
      g_display_mode = line->display_mode;
    }
    
    uint8_t* source_palette = g_scan_bank->bytes + (line->palette_addr * sizeof(uint32_t));
    int palette_mask = line->palette_mask;
    for (int i = 0; i < 16; i += 4) {
      if (palette_mask & 1) {
        for (int j = 0; j < 4; ++j) {
          g_palette[i + j] = source_palette[i + j];
        }
      }
      palette_mask >>= 1;
    }

    if (line->pixels_addr_en) {
      g_pixels_addr = line->pixels_addr;
    }

    if (line->x_shift_en) {
      g_x_shift = line->x_shift;
    }

    g_lines_enabled = line->next_line_en;
  }

  g_display_blit_clock_enabled = g_blit_bank != g_scan_bank && g_display_mode != DISPLAY_MODE_LORES_256 && g_display_mode != DISPLAY_MODE_HIRES_16;
  
  bool border_left = g_sys80_regs.border_left * 2;
  bool border_right = g_sys80_regs.border_left * 2;
  uint8_t border_rgb = g_sys80_regs.border_rgb;

  switch (g_display_mode) {
    case DISPLAY_MODE_DISABLED:
      ScanOutSolid(dest + g_x_shift, width, g_palette[0]);
      break;
    case DISPLAY_MODE_HIRES_2:
      ScanOutHires2(dest + g_x_shift, width);
      break;
    case DISPLAY_MODE_HIRES_4:
      ScanOutHires4(dest + g_x_shift, width);
      break;
    case DISPLAY_MODE_LORES_2:
      ScanOutLores2(dest + g_x_shift, width);
      break;
    case DISPLAY_MODE_LORES_4:
      ScanOutLores4(dest + g_x_shift, width);
      break;
    case DISPLAY_MODE_LORES_16:
      ScanOutLores16(dest + g_x_shift, width);
      break;
    case DISPLAY_MODE_HIRES_16:
      ScanOutHires16(dest + g_x_shift, width);
      break;
    case DISPLAY_MODE_LORES_256:
      ScanOutLores256(dest + g_x_shift, width);
      break;
    case DISPLAY_MODE_LORES_TEXT:
      ScanOutLoresText(dest + g_x_shift, width, y);
      break;
    case DISPLAY_MODE_HIRES_TEXT:
      ScanOutHiresText(dest + g_x_shift, width, y);
      break;
    default:
      ScanOutSolid(dest + g_x_shift, width, AWFUL_MAGENTA);
      break;
  }

  if (g_sprite_cycle <= g_sys80_regs.sprite_duty) {
    int sprite_y = g_sys80_regs.sprite_y;
    if (y >= sprite_y && y < sprite_y + 8) {
      ScanOutSprite(dest + g_x_shift, width, y - sprite_y);
    }
  }

  for (int i = 0; i < g_x_shift; ++i) {
    dest[i] = g_palette[0];
  }
  for (int i = width + g_x_shift; i < width; ++i) {
    dest[i] = g_palette[0];
  }

  memset(dest, border_rgb, border_left);
  memset(dest + width - border_right, border_rgb, border_right);
}

void STRIPED_SECTION ScanOutEndDisplay() {
  g_display_blit_clock_enabled = true;
}

void PutPixel(int x, int y, int color) {
  int word_idx, bit_idx, mask, shifted;
  ScanLine* line = g_scan_bank->lines + y;

  uint32_t* pixels = &g_scan_bank->words[line->pixels_addr];

  switch (line->display_mode) {
    case DISPLAY_MODE_LORES_2:
    case DISPLAY_MODE_HIRES_2:
      word_idx = x >> 5;
      bit_idx = x & 0x1F;
      mask = 0x1 << bit_idx;
      shifted = color << bit_idx;
      pixels[word_idx] = (pixels[word_idx] & ~mask) | (shifted & mask);
      break;
    case DISPLAY_MODE_LORES_4:
    case DISPLAY_MODE_HIRES_4:
      word_idx = x >> 4;
      bit_idx = x & 0xF;
      mask = 0x3 << (bit_idx << 1);
      shifted = color << (bit_idx << 1);
      pixels[word_idx] = (pixels[word_idx] & ~mask) | (shifted & mask);
      break;
    case DISPLAY_MODE_LORES_16:
      word_idx = x >> 3;
      bit_idx = x & 0x7;
      mask = 0xF << (bit_idx << 2);
      shifted = color << (bit_idx << 2);
      pixels[word_idx] = (pixels[word_idx] & ~mask) | (shifted & mask);
      break;
    default:
      assert(false);
  }
}

int GetDisplayModeBPP(DisplayMode mode) {
  switch (mode) {
    case DISPLAY_MODE_HIRES_2:
      return 1;
    case DISPLAY_MODE_HIRES_4:
      return 2;
    case DISPLAY_MODE_HIRES_16:
      return 4;
    case DISPLAY_MODE_LORES_2:
      return 1;
    case DISPLAY_MODE_LORES_4:
      return 2;
    case DISPLAY_MODE_LORES_16:
      return 4;
    case DISPLAY_MODE_LORES_256:
      return 8;
    default:
      assert(false);
      return 0;
  }
}

void InitScanOutTest(DisplayMode mode, int width, int height) {
  if (mode >= DISPLAY_MODE_LORES_2) {
    width /= 2;
  }

  int width_words = width * GetDisplayModeBPP(mode) / 32;

  unsigned pixels_addr = offsetof(DisplayBank, lines[height]) / sizeof(uint32_t);

  for (int y = 0; y < height; ++y) {
    ScanLine* line = g_scan_bank->lines + y;
    line->palette_mask = 0;
    line->display_mode = mode;
    line->display_mode_en = true;
    line->pixels_addr = pixels_addr;
    line->pixels_addr_en = true;
    line->x_shift = ~y;
    pixels_addr += width_words;
  }

  int color = 0;
  for (int cy = 0; cy < height; cy += 16) {
    for (int cx = 0; cx < width; cx += 16) {
      for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
          PutPixel(cx + x, cy + y, 1);
          if (x < 4) {
            PutPixel(cx + x + 8, cy + y, y);
          } else {
            PutPixel(cx + x + 8, cy + y, y + 8);
          }
          PutPixel(cx + x, cy + y + 8, 0);
          PutPixel(cx + x + 8, cy + y + 8, 1);
        }
      }
    }
  }

  for (int y = 1; y < height; ++y) {
    ScanLine* line = g_scan_bank->lines + y;
    line->pixels_addr = 0;
    line->pixels_addr_en = false;
    line->display_mode_en = false;
  }
}
