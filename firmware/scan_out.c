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

#define NUM_ACTIVE_SPRITES 8
#define NUM_SPRITE_PRIORITIES 4

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
      uint16_t tile_idx   : 11;
      uint8_t palette_idx : 3;
      bool flip_x         : 1;
      bool flip_y         : 1;
    } tile;

    struct {
      uint8_t char_idx : 8;
      uint8_t fg_color : 4;
      uint8_t bg_color : 4;
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
    uint8_t bytes[65536];
    uint32_t tiles[65536 / sizeof(uint32_t)];
  };
} Plane;

typedef struct {
  uint8_t pad[128];
  uint8_t palettes[NUM_PALETTES][PALETTE_SIZE];   // 128 bytes
} SpriteRegs;

static_assert(sizeof(SpriteRegs) == 256);

typedef struct {
  int16_t x: 10;
  int16_t y: 9;

  uint8_t height;  // height + 1

  uint8_t width: 1;   // 0->16 or 1->32
  uint8_t priority: 2;
  bool transparent: 1;
  uint8_t palette_idx: 3;

  uint16_t image_addr: 14;
} Sprite;

static_assert(sizeof(Sprite) == 8);

typedef struct {
  Sprite sprite;
} ActiveSprite;

static ActiveSprite g_active_sprites[NUM_ACTIVE_SPRITES];
static int g_pending_sprite_idx;

typedef struct {
  union {
    struct {
      SpriteRegs regs;
      Sprite sprites[256];
    };
    uint8_t bytes[65536];
    uint32_t images[65536 / sizeof(uint32_t)];
  };
} SpriteLayer;

static_assert(sizeof(SpriteLayer) == 65536);

static Plane g_planes[NAM_PLANES];
static PlaneRegs g_plane_regs[NAM_PLANES];

static SpriteLayer g_sprite_layer;
static uint16_t g_sprite_palettes[NUM_PALETTES][PALETTE_SIZE];

int STRIPED_SECTION ReadVideoMemByte(int device, int address) {
  switch (device) {
  case 0:
  case 1:
    return g_planes[device].bytes[address & 0xFFFF];
  default:
    return 0;
  }
}

void STRIPED_SECTION WriteVideoMemByte(int device, int address, int data) {
  switch (device) {
  case 0:
  case 1:
    g_planes[device].bytes[address & 0xFFFF] = data;
  }
}

#pragma GCC push_options
#pragma GCC optimize("O3")

static void SCAN_OUT_INNER_SECTION ScanOutSolid(uint8_t* dest, int width, uint8_t rgb) {
  for (int x = 0; x < width; ++x) {
    *dest++ = rgb;
  }
}

static void STRIPED_SECTION ConfigureInterpolator(int shift_bits, int mask_bits) {
  interp_config cfg;
  
  cfg = interp_default_config();
  interp_config_set_shift(&cfg, shift_bits);
  interp_set_config(interp0, 0, &cfg);
  interp_set_config(interp1, 0, &cfg);

  cfg = interp_default_config();
  interp_config_set_cross_input(&cfg, true);
  interp_config_set_mask(&cfg, 0, mask_bits - 1);
  interp_set_config(interp0, 1, &cfg);
  interp_set_config(interp1, 1, &cfg);
}

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

typedef struct {
  ParallelContext parallel;
  uint8_t* dest;
  int y;
} PlaneContext;

static void SCAN_OUT_INNER_SECTION ScanOutTileMode(const PlaneContext* ctx, int core_num, int plane_idx) {
  const Plane* plane = &g_planes[plane_idx];
  const PlaneRegs* regs = &g_plane_regs[plane_idx];
  int y = ctx->y + regs->window_y;
  int pattern_y = y & (TILE_SIZE-1);
  
  int source_idx = (y * TILE_SIZE) * PLANE_WIDTH + (regs->window_x * TILE_SIZE) + core_num;

  static_assert(TILE_SIZE * 2 == 16);
  uint8_t* dest = ctx->dest - (regs->window_x & (TILE_SIZE-1)) + (core_num * TILE_SIZE * 2);

  ConfigureInterpolator(4, 4);

  for (int c = 0; c < 41; c += NUM_CORES) {
    Name name = GetName(plane, source_idx);
    source_idx += NUM_CORES;

    const uint8_t* palette = GetPalette(regs, name.tile.palette_idx);

    int flipped_pattern_y = pattern_y;
    if (name.tile.flip_y) {
      flipped_pattern_y = (TILE_SIZE-1) - flipped_pattern_y;
    }

    int tile_idx = name.tile.tile_idx;
    interp0->accum[0] = GetTile(plane, tile_idx * TILE_WORDS + flipped_pattern_y);

    #pragma GCC unroll 8
    for (int i = 0; i < 16; i += 2) {
      int color = interp0->pop[1];
      dest[i] = dest[i+1] = palette[color];
    }

    dest += 32;
  }
}

static void SCAN_OUT_INNER_SECTION ScanOutTextMode(const PlaneContext* ctx, int core_num, int plane_idx, bool hires, int text_height) {
  const Plane* plane = &g_planes[plane_idx];
  const PlaneRegs* regs = &g_plane_regs[plane_idx];

  int y = ctx->y + regs->window_y * text_height;
  int32_t pattern_y = y % text_height;
  int char_y = y / text_height;

  int source_idx = (char_y * PLANE_WIDTH) + regs->window_x + core_num;

  const uint8_t* palette = GetPalette(regs, 0);

  static_assert(TEXT_WIDTH * 2 == 16);
  uint8_t* dest = ctx->dest + (core_num * TEXT_WIDTH * 2);
  if (hires) {
    dest += plane_idx * TEXT_WIDTH;
  }

  ConfigureInterpolator(1, 1);

  for (int c = 0; c < 41; c += NUM_CORES) {
    Name name = GetName(plane, source_idx);
    source_idx += NUM_CORES;

    int char_idx = name.text.char_idx;
    interp0->accum[0] = GetChar(plane, char_idx * CHAR_BYTES + pattern_y);

    uint8_t colors[2] = {
      palette[name.text.bg_color],
      palette[name.text.fg_color],
    };

    if (hires) {
      #pragma GCC unroll 8
      for (int i = 7; i >= 0; --i) {
        int color = interp0->pop[1];
        dest[i] = colors[color];
      }
    } else {
      #pragma GCC unroll 8
      for (int i = 14; i >= 0; i -= 2) {
        int color = interp0->pop[1];
        dest[i] = dest[i+1] = colors[color];
      }
    }

    dest += 32;
  }
}


typedef struct {
  ParallelContext parallel;
  uint8_t* dest;
  int y;
} SpriteContext;

static void SCAN_OUT_INNER_SECTION ScanOutSprites(const void* cc, int core_num) {
  const SpriteContext* ctx = cc;
  int y = ctx->y;

  ConfigureInterpolator(4 * NUM_CORES, 4);

  for (int priority = NUM_SPRITE_PRIORITIES - 1; priority > 0; --priority) {
    for (int j = 0; j < count_of(g_active_sprites); ++j) {
      const ActiveSprite* active = &g_active_sprites[j];
      if (active->sprite.priority != priority)
        continue;
      if (active->sprite.y > y)
        continue;
      if (active->sprite.x <= -32 || active->sprite.x >= 320)
        continue;

      int width = active->sprite.width ? 4 : 2;
      const uint32_t* src = g_sprite_layer.images + active->sprite.image_addr + width * (y - active->sprite.y);
      width *= 8; // 16 or 32

      const uint16_t *palette = g_sprite_palettes[active->sprite.palette_idx];
      uint16_t* dest = ((uint16_t*) ctx->dest) + active->sprite.x;

      int x = (active->sprite.x & (NUM_CORES-1)) ^ core_num;
      if (active->sprite.transparent) {
        for (; x < width; x += 8) {
          interp0->accum[0] = (*src++) >> (core_num * 4);

          #pragma GCC unroll 4
          for (int i = 6; i >= 0; i -= 2) {
            int color = interp0->pop[1];
            if (color != 0) {
              dest[x + i] = palette[color];
            }
          }
        }
      } else {
        for (; x < width; x += 8) {
          interp0->accum[0] = (*src++) >> (core_num * 4);

          #pragma GCC unroll 4
          for (int i = 6; i >= 0; i -= 2) {
            int color = interp0->pop[1];
            dest[x + i] = palette[color];
          }
        }
      }
    }
  }
}

#pragma GCC pop_options

static void STRIPED_SECTION ScanOutPlanes(const void* cc, int core_num) {
  const PlaneContext* ctx = cc;
  for (int i = 0; i < NAM_PLANES; ++i) {
    const PlaneRegs* regs = &g_plane_regs[i];

    switch (regs->plane_mode) {
    case PLANE_MODE_TILE:
      ScanOutTileMode(ctx, core_num, i);
      break;
    case PLANE_MODE_TEXT_40_30:
      ScanOutTextMode(ctx, core_num, i, false, 8);
      break;
    case PLANE_MODE_TEXT_40_24:
      ScanOutTextMode(ctx, core_num, i, false, 10);
      break;
    case PLANE_MODE_TEXT_80_30:
      ScanOutTextMode(ctx, core_num, i, true, 8);
      break;
    case PLANE_MODE_TEXT_80_24:
      ScanOutTextMode(ctx, core_num, i, true, 10);
      break;
    }
  }
}

static void STRIPED_SECTION UpdateActiveSprites(int y) {
  for (int i = 0; i < NUM_ACTIVE_SPRITES; ++i) {
    ActiveSprite *active = &g_active_sprites[i];

    if (y > active->sprite.y + active->sprite.height) {
      active->sprite = g_sprite_layer.sprites[g_pending_sprite_idx++];
    }
  }
}

void STRIPED_SECTION ScanOutBeginDisplay() {
  for (int i = 0; i < NAM_PLANES; ++i) {
    g_plane_regs[i] = g_planes[i].regs;
  }
  
  for (int i = 0; i < NUM_PALETTES; ++i) {
    for (int j = 0; j < PALETTE_SIZE; ++j) {
      int rgb = g_sprite_layer.regs.palettes[i][j];
      g_sprite_palettes[i][j] = (rgb << 8) | rgb;
    }
  }

  g_pending_sprite_idx = 0;
  for (int i = 0; i < NUM_ACTIVE_SPRITES; ++i) {
    g_active_sprites[i].sprite.y = -1;
    g_active_sprites[i].sprite.height = 0;
  }
}

void STRIPED_SECTION ScanOutLine(uint8_t* dest, int y, int width) {
  g_sys80_regs.current_y = y;

  PlaneContext plane_ctx = {
    .parallel.entry = ScanOutPlanes,
    .dest = dest,
    .y = y,
  };

  Parallel(&plane_ctx);

  UpdateActiveSprites(y);

  SpriteContext sprite_ctx = {
    .parallel.entry = ScanOutSprites,
    .dest = dest,
    .y = y,
  };

  Parallel(&sprite_ctx);
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

  g_planes[0].regs.plane_mode = PLANE_MODE_TILE;
  g_planes[1].regs.plane_mode = PLANE_MODE_TILE;

  memset(g_sprite_layer.bytes, 0x11, sizeof(g_sprite_layer.bytes));

  int i = 0;
  for (int y = 0; y < 8; ++y) {
    for (int x = 0; x < 8; ++x) {
      Sprite* sprite = &g_sprite_layer.sprites[i];
      sprite->height = 31;
      sprite->priority = 1;
      sprite->width = 1;
      sprite->x = x * 35;
      sprite->y = y * 35 + x;
      ++i;
    }
  }

  for (; i < count_of(g_sprite_layer.sprites); ++i) {
    Sprite* sprite = &g_sprite_layer.sprites[i];
    sprite->y = -1;
    sprite->height = 1;
  }
}