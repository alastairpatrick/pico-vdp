#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"

#include "section.h"
#include "video.h"

#include "scan_out.h"

#define DISPLAY_BANK_SIZE (32 * 1024 / sizeof(uint32_t))
#define AWFUL_MAGENTA 0xC7

typedef struct {
  // WORD #0

  // Address/4 of palette for this scanline.
  uint16_t palette_addr: 13;
  int reserved0: 5;

  // Address/4 of of first pixel.
  uint16_t pixels_addr: 13;
  int reserved1: 5;

  // WORD #1

  // Which color-quads in palette to update on this scanline.
  uint8_t palette_mask: 4;
  uint8_t reserved2: 4;

  bool pixels_addr_en: 1;
  DisplayMode display_mode: 3;
  uint8_t reserved3: 4;

  bool x_shift_en: 1;
  uint8_t x_shift: 3;
  uint8_t reserved4: 4;

  uint8_t reserved5: 8;
} ScanLine;

typedef union {
  ScanLine lines[192];
  uint32_t words[DISPLAY_BANK_SIZE];
} DisplayBank;

DisplayBank g_display_bank_a;
DisplayBank g_display_bank_b;
DisplayBank* g_scan_bank = &g_display_bank_a;

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

static int g_pixels_addr;

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

  const uint32_t* source = g_scan_bank->words + g_pixels_addr;

  int x_shift = line->x_shift_en ? line->x_shift : 0;
  for (int i = 0; i < x_shift; ++i) {
    *dest++ = g_palette[0];
  }

  switch (line->display_mode) {
    case DISPLAY_MODE_DISABLED:
      ScanOutSolid(dest, width, g_palette[0]);
      break;
    case DISPLAY_MODE_HIRES_2:
      source = ScanOutHires2(dest, source, width);
      break;
    case DISPLAY_MODE_HIRES_4:
      source = ScanOutHires4(dest, source, width);
      break;
    case DISPLAY_MODE_LORES_2:
      source = ScanOutLores2(dest, source, width);
      break;
    case DISPLAY_MODE_LORES_4:
      source = ScanOutLores4(dest, source, width);
      break;
    case DISPLAY_MODE_LORES_16:
      source = ScanOutLores16(dest, source, width);
      break;
    default:
      source = ScanOutSolid(dest, width, AWFUL_MAGENTA);
      break;
  }

  g_pixels_addr = source - g_scan_bank->words;
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
  const static int x_shifts[8] = { 0, 2, 4, 6, 7, 6, 4, 2 };

  if (mode >= DISPLAY_MODE_LORES_2) {
    width /= 2;
  }

  int width_words = width * GetDisplayModeBPP(mode) / 32;

  unsigned pixels_addr = height * sizeof(ScanLine) / sizeof(uint32_t);

  for (int y = 0; y < height; ++y) {
    ScanLine* line = g_scan_bank->lines + y;
    line->palette_mask = 0;
    line->display_mode = mode;
    line->pixels_addr = pixels_addr;
    line->pixels_addr_en = true;
    line->x_shift = x_shifts[y & 0x7];
    line->x_shift_en = true;
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
  }
}
