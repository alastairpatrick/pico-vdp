#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "hardware/interp.h"
#include "pico.h"
#include "pico/stdlib.h"

#include "scan_out.h"

#include "parallel.h"
#include "section.h"
#include "sys80.h"
#include "video_dma.h"

#define AWFUL_MAGENTA 0xC7

#define PLANE_WIDTH 64
#define PLANE_HEIGHT 64
#define NUM_PALETTES 8
#define PALETTE_SIZE 16
#define NAM_PLANES 2

#define TILE_SIZE 8
#define TILE_WORDS 8

#define TEXT_WIDTH 8
#define CHAR_BYTES 16

typedef struct {
  PlaneMode plane_mode :8;                        // 1 bytes
  uint8_t window_x, window_y;                     // 2 bytes
  uint8_t pad[125];
  uint8_t palettes[NUM_PALETTES][PALETTE_SIZE];   // 128 bytes
} PlaneRegs;

static_assert(sizeof(PlaneRegs) == 256);

typedef struct {
  union {
    struct {
      uint8_t palette_idx : 3;
      bool flip_x         : 1;
      bool flip_y         : 1;
      uint16_t tile_idx   : 11;
    } tile;

    struct {
      uint8_t bg_color : 4;
      uint8_t fg_color : 4;
      uint8_t char_idx : 8;
    } text;
  };
} Name;

static_assert(sizeof(Name) == 2);

typedef struct {
  union {
    struct {
      Name names[PLANE_WIDTH * PLANE_HEIGHT]; // 3840 bytes
      PlaneRegs regs;                         // 256 bytes
      uint8_t chars[256 * CHAR_BYTES];
    };
    uint32_t tiles[65536 / sizeof(uint32_t)];
  };
} Plane;

static_assert(sizeof(Plane) == 65536);

static Plane g_planes[NAM_PLANES];
static PlaneRegs g_plane_regs[NAM_PLANES];

#pragma GCC push_options
#pragma GCC optimize("O3")

static void SCAN_OUT_INNER_SECTION ScanOutSolid(uint8_t* dest, int width, uint8_t rgb) {
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

typedef struct {
  ParallelContext parallel;
  uint8_t* dest;
  int plane_idx;
  int y;
} PlaneContext;

static SCAN_OUT_INNER_SECTION Name GetName(const Plane* plane, int idx) {
  return plane->names[idx & (count_of(plane->names) - 1)];
}

static SCAN_OUT_INNER_SECTION uint32_t GetTile(const Plane* plane, int idx) {
  return plane->tiles[idx & (count_of(plane->tiles) - 1)];
}

static SCAN_OUT_INNER_SECTION int GetChar(const Plane* plane, int idx) {
  return plane->chars[idx & (count_of(plane->chars) - 1)];
}

static SCAN_OUT_INNER_SECTION const uint8_t* GetPalette(const PlaneRegs* regs, int idx) {
  return regs->palettes[idx & (count_of(regs->palettes) - 1)];
}

static void SCAN_OUT_INNER_SECTION ScanOutTileMode(const PlaneContext* ctx, int core_num) {
  const Plane* plane = &g_planes[ctx->plane_idx];
  const PlaneRegs* regs = &g_plane_regs[ctx->plane_idx];
  int y = ctx->y + regs->window_y;

  int source_idx = (y * TILE_SIZE) * PLANE_WIDTH + (regs->window_x * TILE_SIZE) + core_num;

  static_assert(TILE_SIZE * 2 == 16);
  uint8_t* dest = ctx->dest - (regs->window_x & (TILE_SIZE-1)) + (core_num * TILE_SIZE * 2);

  ConfigureInterpolator(4);

  for (int c = 0; c < 41; c += NUM_CORES) {
    Name name = GetName(plane, source_idx);
    source_idx += NUM_CORES;

    const uint8_t* palette = GetPalette(regs, name.tile.palette_idx);

    int pattern_y = y & (TILE_SIZE-1);
    if (name.tile.flip_y) {
      pattern_y = (TILE_SIZE-1) - pattern_y;
    }

    int tile_idx = name.tile.tile_idx;
    interp0->accum[0] = GetTile(plane, tile_idx * TILE_WORDS + pattern_y);

    #pragma GCC unroll 8
    for (int i = 0; i < 16; i += 2) {
      int color = interp0->pop[1];
      dest[i] = dest[i+1] = palette[color];
    }

    dest += 32;
  }
}

static void SCAN_OUT_INNER_SECTION ScanOutTextMode(const PlaneContext* ctx, int core_num, int text_height) {
  const Plane* plane = &g_planes[ctx->plane_idx];
  const PlaneRegs* regs = &g_plane_regs[ctx->plane_idx];

  int y = ctx->y + regs->window_y * text_height;
  int32_t pattern_y = y % text_height;
  int char_y = y / text_height;

  int source_idx = (char_y * PLANE_WIDTH) + regs->window_x + core_num;

  const uint8_t* palette = GetPalette(regs, 0);

  static_assert(TEXT_WIDTH * 2 == 16);
  uint8_t* dest = ctx->dest + (ctx->plane_idx * TEXT_WIDTH) + (core_num * TEXT_WIDTH * 2);

  ConfigureInterpolator(1);

  for (int c = 0; c < 41; c += NUM_CORES) {
    Name name = GetName(plane, source_idx);
    source_idx += NUM_CORES;

    int char_idx = name.text.char_idx;
    interp0->accum[0] = GetChar(plane, char_idx * CHAR_BYTES + pattern_y);

    uint8_t colors[2] = {
      palette[name.text.bg_color],
      palette[name.text.fg_color],
    };

    #pragma GCC unroll 8
    for (int i = 0; i < 8; ++i) {
      int color = interp0->pop[1];
      dest[i] = colors[color];
    }

    dest += 32;
  }
}

#pragma GCC pop_options

static void STRIPED_SECTION ScanOutPlane(const void* cc, int core_num) {
  const PlaneContext* ctx = cc;
  const PlaneRegs* regs = &g_plane_regs[ctx->plane_idx];

  switch (regs->plane_mode) {
  case PLANE_MODE_TILE:
    ScanOutTileMode(ctx, core_num);
    break;
  case PLANE_MODE_TEXT_80_30:
    ScanOutTextMode(ctx, core_num, 8);
    break;
  case PLANE_MODE_TEXT_80_24:
    ScanOutTextMode(ctx, core_num, 10);
    break;
  }
}

void STRIPED_SECTION ScanOutBeginDisplay() {
  for (int i = 0; i < NAM_PLANES; ++i) {
    g_plane_regs[i] = g_planes[i].regs;
  }
}

void STRIPED_SECTION ScanOutLine(uint8_t* dest, int y, int width) {
  g_sys80_regs.current_y = y;

  for (int i = 0; i < NAM_PLANES; ++i) {
    PlaneContext ctx = {
      .parallel.entry = ScanOutPlane,
      .plane_idx = i,
      .dest = dest,
      .y = y,
    };

    Parallel(&ctx);
  }
}

void STRIPED_SECTION ScanOutEndDisplay() {
}

void InitScanOut() {
  for (int i = 0; i < count_of(g_planes->chars); ++i) {
    g_planes[0].chars[i] = rand();
    g_planes[1].chars[i] = rand();
  }

  for (int i = 0; i < 16; ++i) {
    g_planes[0].regs.palettes[0][i] = g_planes[1].regs.palettes[0][i] = rand();
  }

  for (int i = 0; i < count_of(g_planes->names); ++i) {
    for (int j = 0; j < 2; ++j) {
      Name name = {
        .text.bg_color = rand(),
        .text.fg_color = rand(),
        .text.char_idx = i,
      };
      g_planes[j].names[i] = name;
    }
  }

  g_planes[0].regs.plane_mode = g_planes[1].regs.plane_mode = PLANE_MODE_TEXT_80_24;
}