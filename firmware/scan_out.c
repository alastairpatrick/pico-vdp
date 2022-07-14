#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "hardware/interp.h"
#include "pico.h"
#include "pico/stdlib.h"

#include "scan_out.h"

#include "common.h"
#include "parallel.h"
#include "section.h"
#include "sys80.h"
#include "video_dma.h"

#define AWFUL_MAGENTA 0xC7

#define PLANE_WIDTH 64
#define PLANE_HEIGHT 64
#define NUM_PALETTES 8
#define PALETTE_SIZE 16
#define NUM_PLANES 2

#define TILE_SIZE 8
#define TILE_WORDS 8

#define TEXT_WIDTH 8
#define CHAR_BYTES 16

#define NUM_ACTIVE_SPRITES 8
#define NUM_SPRITE_PRIORITIES 4

typedef struct {
  PlaneMode plane_mode :8;                        // 1 bytes
  uint8_t window_x, window_y;                     // 2 bytes
} PlaneRegs;

typedef struct {
  union {
    struct {
      uint8_t palette_idx : 3;
      bool flip_x         : 1;
      bool flip_y         : 1;
      uint16_t tile_idx   : 11;
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
      Name names[PLANE_WIDTH * PLANE_HEIGHT];         // 3840 bytes
      PlaneRegs regs;                                 // 128 bytes
      uint8_t pad[128 - sizeof(PlaneRegs)];
      uint8_t palettes[NUM_PALETTES][PALETTE_SIZE];   // 128 bytes
      uint8_t chars[256 * CHAR_BYTES];
    };
    uint8_t bytes[65536];
    uint32_t tiles[65536 / sizeof(uint32_t)];
  };
} Plane;

typedef struct {
} SpriteRegs;

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
      SpriteRegs regs;                                // 128 bytes
      uint8_t pad[128 - sizeof(SpriteRegs)];
      uint8_t palettes[NUM_PALETTES][PALETTE_SIZE];   // 128 bytes
      Sprite sprites[256];
    };
    uint8_t bytes[65536];
    uint32_t images[65536 / sizeof(uint32_t)];
  };
} SpriteLayer;

static_assert(sizeof(SpriteLayer) == 65536);

typedef uint16_t Palette16[PALETTE_SIZE];

static Plane g_planes[NUM_PLANES];
static PlaneRegs g_plane_regs[NUM_PLANES];
static Palette16 g_plane_palettes[NUM_PLANES][NUM_PALETTES];

static SpriteLayer g_sprite_layer;
static Palette16 g_sprite_palettes[NUM_PALETTES];

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

static void STRIPED_SECTION ConfigureInterpolator(int shift_bits, int mask_lsb, int mask_msb, bool sign_extend) {
  interp_config cfg;
  
  cfg = interp_default_config();
  interp_config_set_shift(&cfg, shift_bits);
  interp_set_config(interp0, 0, &cfg);
  
  cfg = interp_default_config();
  interp_config_set_cross_input(&cfg, true);
  interp_config_set_mask(&cfg, mask_lsb, mask_msb);
  interp_config_set_signed(&cfg, sign_extend);
  interp_set_config(interp0, 1, &cfg);
  
  interp0->base[1] = 0;
}

static SCAN_OUT_INNER_SECTION Name GetName(const Plane* plane, int idx) {
  return plane->names[idx & (count_of(plane->names) - 1)];
}

static SCAN_OUT_INNER_SECTION uint32_t GetTile(const Plane* plane, int idx) {
  return plane->tiles[idx & (count_of(plane->tiles) - 1)];
}

static void SCAN_OUT_INNER_SECTION WriteDoublePixel(uint8_t* dest, int rgb) {
  // This is the proper way to do type punning but even with -O3 it still generates a call to memcpy()
  //memcpy(dest, &rgb, 2);

  // Incorrect type punning.
  *(uint16_t*) dest = rgb;
}

static int SCAN_OUT_INNER_SECTION PreparePalette(const uint16_t* palette) {
  int half_palette = ((int) palette) >> 1;
  interp0->base[1] = half_palette;
  return half_palette;
}

static int SCAN_OUT_INNER_SECTION LookupPalette(int color) {
  int rgb;
  asm ("LDRH %0, [%1, %1]" : "=l" (rgb) : "l" (color));
  return rgb;
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

  const Palette16* palette = g_plane_palettes[plane_idx];
  
  ConfigureInterpolator(4, 0, 3, false);

  for (int c = 0; c < 41; c += NUM_CORES) {
    Name name = GetName(plane, source_idx);
    source_idx += NUM_CORES;

    int transparent = PreparePalette(palette[name.tile.palette_idx & (NUM_PALETTES-1)]);

    int flipped_pattern_y = pattern_y;
    //if (name.tile.flip_y) {
    //  flipped_pattern_y = (TILE_SIZE-1) - flipped_pattern_y;
    //}

    int tile_idx = name.tile.tile_idx;
    interp0->accum[0] = GetTile(plane, tile_idx * TILE_WORDS + flipped_pattern_y);

    if (plane_idx) {
      #pragma GCC unroll 8
      for (int i = 0; i < 16; i += 2) {
        int color = interp0->pop[1];
        if (color != transparent) {
          WriteDoublePixel(dest + i, LookupPalette(color));
        }
      }
    } else {
      #pragma GCC unroll 8
      for (int i = 0; i < 16; i += 2) {
        int color = interp0->pop[1];
        WriteDoublePixel(dest + i, LookupPalette(color));
      }
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

  const uint16_t* palette = g_plane_palettes[plane_idx][0];

  static_assert(TEXT_WIDTH * 2 == 16);
  uint8_t* dest = ctx->dest + (core_num * TEXT_WIDTH * 2);
  if (hires) {
    dest += plane_idx * TEXT_WIDTH;
  }

  if (hires) {
    ConfigureInterpolator(1, 0, 0, false);

    for (int c = 0; c < 41; c += NUM_CORES) {
      Name name = GetName(plane, source_idx);
      source_idx += NUM_CORES;

      interp0->accum[0] = plane->chars[name.text.char_idx * CHAR_BYTES + pattern_y];

      uint8_t local_palette[] = {
        palette[name.text.bg_color],
        palette[name.text.fg_color],
      };

      #pragma GCC unroll 8
      for (int i = 7; i >= 0; --i) {
        dest[i] = local_palette[interp0->pop[1]];
      }

      dest += 32;
    }
  } else {
    ConfigureInterpolator(1, 1, 1, false);

    for (int c = 0; c < 41; c += NUM_CORES) {
      Name name = GetName(plane, source_idx);
      source_idx += NUM_CORES;

      interp0->accum[0] = plane->chars[name.text.char_idx * CHAR_BYTES + pattern_y] << 1;

      uint16_t local_palette[] = {
        palette[name.text.bg_color],
        palette[name.text.fg_color],
      };

      #pragma GCC unroll 8
      for (int i = 14; i >= 0; i -= 2) {
        int color = *(uint16_t*) (((uint8_t*) local_palette) + interp0->pop[1]);
        WriteDoublePixel(dest + i, color);
      }

      dest += 32;
    }
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

  ConfigureInterpolator(4 * NUM_CORES, 0, 3, false);

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

      int transparent = PreparePalette(g_sprite_palettes[active->sprite.palette_idx]);
      if (!active->sprite.transparent) {
        transparent = -1;
      }

      uint16_t* dest = ((uint16_t*) ctx->dest) + active->sprite.x;

      int x = (active->sprite.x & (NUM_CORES-1)) ^ core_num;
      for (; x < width; x += 8) {
        interp0->accum[0] = (*src++) >> (core_num * 4);

        #pragma GCC unroll 4
        for (int i = 6; i >= 0; i -= 2) {
          int color = interp0->pop[1];
          if (color != transparent) {
            dest[x + i] = LookupPalette(color);
          }
        }
      }
    }
  }
}

#pragma GCC pop_options

static void STRIPED_SECTION ScanOutPlanes(const void* cc, int core_num) {
  const PlaneContext* ctx = cc;
  for (int i = 0; i < NUM_PLANES; ++i) {
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

static void STRIPED_SECTION DoublePalettes(Palette16 dest[], uint8_t source[][PALETTE_SIZE], int num) {
  for (int i = 0; i < num; ++i) {
    for (int j = 0; j < PALETTE_SIZE; ++j) {
      int rgb = source[i][j];
      dest[i][j] = (rgb << 8) | rgb;
    }
  }
}

void STRIPED_SECTION ScanOutBeginDisplay() {
  for (int i = 0; i < NUM_PLANES; ++i) {
    g_plane_regs[i] = g_planes[i].regs;

    DoublePalettes(g_plane_palettes[i], g_planes[i].palettes, NUM_PALETTES);
  }
  
  DoublePalettes(g_sprite_palettes, g_sprite_layer.palettes, NUM_PALETTES);

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
    g_planes[1].chars[i] = 0;
  }

  for (int i = 0; i < 16; ++i) {
    g_planes[0].palettes[0][i] = g_planes[1].palettes[0][i] = rand();
  }

  for (int i = 0; i < count_of(g_planes->names); ++i) {
    for (int j = 0; j < 2; ++j) {
      Name name = {0};
      g_planes[1].names[i] = name;

      name.tile.tile_idx = rand();
      g_planes[0].names[i] = name;
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