#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "hardware/interp.h"
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
static int g_start_line;

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
  if (dot_x < g_blank_logical_width) {
    return true;
  } else {
    return g_display_blit_clock_enabled;
  }
}

void STRIPED_SECTION SwapBanks(SwapMode mode, int line_idx) {
  g_swap_mode = mode;
  g_sys80_regs.start_line = line_idx;
  g_swap_pending = true;
}

bool STRIPED_SECTION IsSwapPending() {
  return g_swap_pending;
}

DisplayBank* STRIPED_SECTION GetBlitBank() {
  return g_blit_bank;
}

#pragma GCC push_options
#pragma GCC optimize("O3")

void SCAN_OUT_INNER_SECTION ScanOutSolid(uint8_t* dest, int width, uint8_t rgb) {
  for (int x = 0; x < width; ++x) {
    *dest++ = rgb;
  }
}

static void STRIPED_SECTION ConfigureInterpolator(int shift_bits) {
  interp_config cfg;
  
  cfg = interp_default_config();
  interp_config_set_shift(&cfg, shift_bits);
  interp_set_config(interp0, 0, &cfg);
  interp_set_config(interp1, 0, &cfg);

  cfg = interp_default_config();
  interp_config_set_cross_input(&cfg, true);
  interp_config_set_mask(&cfg, 0, shift_bits - 1);
  interp_set_config(interp0, 1, &cfg);
  interp_set_config(interp1, 1, &cfg);
}

static uint32_t SCAN_OUT_INNER_SECTION ReadPixelData() {
  uint32_t data = g_scan_bank->words[g_pixels_addr & (DISPLAY_BANK_SIZE-1)];
  ++g_pixels_addr;
  return data;
}

static void SCAN_OUT_INNER_SECTION ScanOutLores2(uint8_t* dest, int width) {
  ConfigureInterpolator(1);

  for (int x = 0; x < width/64; ++x) {
    interp0->accum[0] = ReadPixelData();

    for (int i = 0; i < 32; ++i) {
      *dest++ = *dest++ = g_palette[interp0->pop[1]];
    }
  }
}

static void SCAN_OUT_INNER_SECTION ScanOutHires2(uint8_t* dest, int width) {
  ConfigureInterpolator(1);

  for (int x = 0; x < width/32; ++x) {
    interp0->accum[0] = ReadPixelData();

    #pragma GCC unroll 4
    for (int i = 0; i < 32; ++i) {
      dest[i] = g_palette[interp0->pop[1]];
    }
    dest += 32;
  }
}

static void SCAN_OUT_INNER_SECTION ScanOutLores4(uint8_t* dest, int width) {
  ConfigureInterpolator(1);

  for (int x = 0; x < width/32; ++x) {
    interp0->accum[0] = ReadPixelData();

    for (int i = 0; i < 16; ++i) {
      *dest++ = *dest++ = g_palette[interp0->pop[1]];
    }
  }
}

static void SCAN_OUT_INNER_SECTION ScanOutHires4(uint8_t* dest, int width) {
  ConfigureInterpolator(2);

  for (int x = 0; x < width/16; ++x) {
    interp0->accum[0] = ReadPixelData();

    #pragma GCC unroll 4
    for (int i = 0; i < 16; ++i) {
      dest[i] = g_palette[interp0->pop[1]];
    }
    dest += 16;
  }
}

static void SCAN_OUT_INNER_SECTION ScanOutLores16(uint8_t* dest, int width) {
  ConfigureInterpolator(4);

  for (int x = 0; x < width/16; ++x) {
    interp0->accum[0] = ReadPixelData();

    for (int i = 0; i < 8; ++i) {
      *dest++ = *dest++ = g_palette[interp0->pop[1]];
    }
  }
}

static void SCAN_OUT_INNER_SECTION ScanOutHires16(uint8_t* dest, int width) {
  ConfigureInterpolator(4);

  for (int x = 0; x < width/16; ++x) {
    interp0->accum[0] = g_display_bank_a.words[g_pixels_addr & (DISPLAY_BANK_SIZE-1)];
    interp1->accum[0] = g_display_bank_b.words[g_pixels_addr & (DISPLAY_BANK_SIZE-1)];
    ++g_pixels_addr;

    for (int i = 0; i < 8; ++i) {
      *dest++ = g_palette[interp0->pop[1]];
      *dest++ = g_palette[interp1->pop[1]];
    }
  }
}

static void SCAN_OUT_INNER_SECTION ScanOutLores256(uint8_t* dest, int width) {
  ConfigureInterpolator(8);

  for (int x = 0; x < width/8; ++x) {
    interp0->accum[0] = g_display_bank_a.words[g_pixels_addr & (DISPLAY_BANK_SIZE-1)];
    interp0->accum[1] = g_display_bank_b.words[g_pixels_addr & (DISPLAY_BANK_SIZE-1)];
    ++g_pixels_addr;

    for (int i = 0; i < 4; ++i) {
      *dest++ = interp0->pop[1];
      *dest++ = interp1->pop[1];
    }
  }
}

void SCAN_OUT_INNER_SECTION ScanOutSprite(uint8_t* dest, int width, int y) {
  int sprite_x = g_sys80_regs.sprite_x * 2;
  int sprite_rgb = g_sys80_regs.sprite_rgb;

  int sprite_bits = g_sys80_regs.sprite_bitmap[y*2] | (g_sys80_regs.sprite_bitmap[y*2+1] << 8);

  #pragma GCC unroll 4
  for (int x = 0; x < 16; ++x) {
    if ((sprite_bits >> x) & 1) {
      dest[sprite_x + x] = sprite_rgb;
    }
  }
}

#pragma GCC pop_options

void STRIPED_SECTION ScanOutBeginDisplay() {
  g_start_line = g_sys80_regs.start_line;

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
  g_sys80_regs.current_y = y;

  if (g_lines_enabled) {
    const int max_lines = 256;
    int lines_high = (g_sys80_regs.lines_page & DISPLAY_PAGE_MASK) * DISPLAY_PAGE_SIZE;
    int lines_low = ((g_start_line + y) & 0xFF) * 2;
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
