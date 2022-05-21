#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"

#include "section.h"
#include "sys80.h"
#include "video.h"

#include "scan_out.h"

#define AWFUL_MAGENTA 0xC7

static DisplayBank g_display_bank_a;
static DisplayBank g_display_bank_b;
static DisplayBank* g_scan_bank = &g_display_bank_a;
DisplayBank* g_blit_bank = &g_display_bank_a;  // TODO: should point to the bank that is not the scan bank

static ScanRegisters g_scan_regs;
static int g_pixels_addr;
static DisplayMode g_display_mode;

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

const uint32_t* SCAN_OUT_INNER_SECTION ScanOutSolid(uint8_t* dest, int width, uint8_t rgb) {
  for (int x = 0; x < width; ++x) {
    *dest++ = rgb;
  }
}

const uint32_t* SCAN_OUT_INNER_SECTION ScanOutLores2(uint8_t* dest, const uint32_t* source, int width) {
  for (int x = 0; x < width/64; ++x) {
    uint32_t indices32 = *source++;

    for (int i = 0; i < 32; ++i) {
      *dest++ = *dest++ = g_palette[indices32 & 0x1];
      indices32 >>= 1;
    }
  }

  return source;
}

const uint32_t* SCAN_OUT_INNER_SECTION ScanOutHires2(uint8_t* dest, const uint32_t* source, int width) {
  for (int x = 0; x < width/32; ++x) {
    uint32_t indices32 = *source++;

    #pragma GCC unroll 4
    for (int i = 0; i < 32; ++i) {
      *dest++ = g_palette[indices32 & 0x1];
      indices32 >>= 1;
    }
  }

  return source;
}

const uint32_t* SCAN_OUT_INNER_SECTION ScanOutLores4(uint8_t* dest, const uint32_t* source, int width) {
  for (int x = 0; x < width/32; ++x) {
    uint32_t indices16 = *source++;

    for (int i = 0; i < 16; ++i) {
      *dest++ = *dest++ = g_palette[indices16 & 0x3];
      indices16 >>= 2;
    }
  }

  return source;
}

const uint32_t* SCAN_OUT_INNER_SECTION ScanOutHires4(uint8_t* dest, const uint32_t* source, int width) {
  for (int x = 0; x < width/16; ++x) {
    uint32_t indices16 = *source++;

    for (int i = 0; i < 16; ++i) {
      *dest++ = g_palette[indices16 & 0x3];
      indices16 >>= 2;
    }
  }

  return source;
}

const uint32_t* SCAN_OUT_INNER_SECTION ScanOutLores16(uint8_t* dest, const uint32_t* source, int width) {
  for (int x = 0; x < width/16; ++x) {
    uint32_t indices8 = *source++;

    for (int i = 0; i < 8; ++i) {
      *dest++ = *dest++ = g_palette[indices8 & 0xF];
      indices8 >>= 4;
    }
  }

  return source;
}

void STRIPED_SECTION ScanOutReset() {
  g_scan_regs = g_scan_bank->regs;

  // TODO: this is just for testing.
  static int count;
  static int dir = 1;
  if (count == 0) {
    if (g_scan_bank->regs.x_shift == 7) {
      dir = -1;
    }
    if (g_scan_bank->regs.x_shift == -8) {
      dir = 1;
    }
    g_scan_bank->regs.x_shift += dir;
  }
  count = (count+1) % 5;
}

void STRIPED_SECTION ScanOutLine(uint8_t* dest, int y, int width) {
  ScanLine* line = g_scan_bank->lines + y;

  uint32_t* source_palette = g_scan_bank->words + line->palette_addr;
  int palette_mask = line->palette_mask;
  for (int i = 0; i < 4; ++i) {
    if (palette_mask & 1) {
      g_palette[i] = source_palette[i];
    }
    palette_mask >>= 1;
  }

  if (line->pixels_addr_en) {
    g_pixels_addr = line->pixels_addr;
  }

  if (line->display_mode_en) {
    g_display_mode = line->display_mode;
  }

  bool border_left = g_sys80_regs.border_left * 2;
  bool border_right = g_sys80_regs.border_left * 2;
  uint8_t border_rgb = g_sys80_regs.border_rgb;

  int x_shift = g_scan_regs.x_shift + line->x_shift;

  const uint32_t* source = g_scan_bank->words + g_pixels_addr;
  switch (g_display_mode) {
    case DISPLAY_MODE_DISABLED:
      ScanOutSolid(dest + x_shift, width, g_palette[0]);
      break;
    case DISPLAY_MODE_HIRES_2:
      source = ScanOutHires2(dest + x_shift, source, width);
      break;
    case DISPLAY_MODE_HIRES_4:
      source = ScanOutHires4(dest + x_shift, source, width);
      break;
    case DISPLAY_MODE_LORES_2:
      source = ScanOutLores2(dest + x_shift, source, width);
      break;
    case DISPLAY_MODE_LORES_4:
      source = ScanOutLores4(dest + x_shift, source, width);
      break;
    case DISPLAY_MODE_LORES_16:
      source = ScanOutLores16(dest + x_shift, source, width);
      break;
    default:
      source = ScanOutSolid(dest + x_shift, width, AWFUL_MAGENTA);
      break;
  }
  g_pixels_addr = source - g_scan_bank->words;

  for (int i = 0; i < x_shift; ++i) {
    dest[i] = g_palette[0];
  }
  for (int i = width + x_shift; i < width; ++i) {
    dest[i] = g_palette[0];
  }

  memset(dest, border_rgb, border_left);
  memset(dest + width - border_right, border_rgb, border_right);
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
  g_scan_bank->regs.x_shift = 0;

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
