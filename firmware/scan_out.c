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

#define SPRITE_WIDTH 16
#define NUM_ACTIVE_SPRITES 16
#define NUM_SPRITE_PRIORITIES 4

typedef struct {
  PlaneMode plane_mode :8;                        // 1 bytes
  uint8_t window_x, window_y;                     // 2 bytes
} PlaneRegs;

typedef struct {
  union {
    struct {
      uint16_t tile_idx   : 11;
      uint8_t palette_idx : 2;
      uint8_t flip_y      : 3;
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
  bool flip_x: 1;
  int16_t y: 10;
  bool flip_y: 1;
  uint8_t z: 2;

  uint16_t image_addr: 10;
  uint8_t height: 2;  // 8 << height
  bool transparent: 1;
  uint8_t palette_idx: 3;
} Sprite;

static_assert(sizeof(Sprite) == 6);

typedef struct {
  int16_t x;
  int16_t y;

  uint8_t z;
  uint8_t palette_idx;

  uint16_t image_addr;

  uint8_t height;
  bool flip_x: 1;
  bool flip_y: 1;
  bool transparent: 1;
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

// -fno-strict-aliasing because the usual memcpy() approach to type punning does not work; the
// compiler generates a call to memcpy() rather than inlining even when just copying a single uint16_t.
#pragma GCC optimize("O3,no-strict-aliasing")

static int STRIPED_SECTION CalcSpriteHeight(int h) {
  return 8 << h;
}

static void STRIPED_SECTION ScanOutSolid(uint8_t* dest, int width, uint8_t rgb) {
  for (int x = 0; x < width; ++x) {
    *dest++ = rgb;
  }
}

static void STRIPED_SECTION ConfigureInterpolator(int shift_bits, int mask_lsb, int mask_msb) {
  interp0->ctrl[0] = shift_bits << SIO_INTERP0_CTRL_LANE0_SHIFT_LSB |
    (31 << SIO_INTERP0_CTRL_LANE0_MASK_MSB_LSB);

  interp0->ctrl[1] = SIO_INTERP0_CTRL_LANE0_CROSS_INPUT_BITS |
    (mask_lsb << SIO_INTERP0_CTRL_LANE0_MASK_LSB_LSB) |
    (mask_msb << SIO_INTERP0_CTRL_LANE0_MASK_MSB_LSB);
  
  interp0->base[1] = 0;
}

static STRIPED_SECTION Name GetName(const Plane* plane, int idx) {
  return plane->names[idx & (count_of(plane->names) - 1)];
}

static void STRIPED_SECTION WriteDoublePixel(uint8_t* dest, int rgb) {
  *(uint16_t*) dest = rgb;
}

static int STRIPED_SECTION PreparePalette(const uint16_t* palette) {
  int half_palette = ((int) palette) >> 1;
  interp0->base[1] = half_palette;
  return half_palette;
}

static int STRIPED_SECTION LookupPalette(int color) {
  int rgb;
  asm ("LDRH %0, [%1, %1]" : "=l" (rgb) : "l" (color));
  return rgb;
}

typedef struct {
  ParallelContext parallel;
  uint8_t* dest;
  int y;
} PlaneContext;

static void STRIPED_SECTION ScanOutTileMode(const PlaneContext* ctx, int core_num, bool transparent, int plane_idx) {
  const Plane* plane = &g_planes[plane_idx];
  const PlaneRegs* regs = &g_plane_regs[plane_idx];
  int y = ctx->y + regs->window_y;
  int pattern_y = y & (TILE_SIZE-1);
  
  int source_idx = (y * TILE_SIZE) * PLANE_WIDTH + (regs->window_x * TILE_SIZE) + core_num;

  static_assert(TILE_SIZE * 2 == 16);
  uint8_t* dest = ctx->dest - (regs->window_x & (TILE_SIZE-1)) + (core_num * TILE_SIZE * 2);

  const Palette16* palette = g_plane_palettes[plane_idx];
  
  ConfigureInterpolator(4, 0, 3);

  for (int c = 0; c < 41; c += NUM_CORES) {
    Name name = GetName(plane, source_idx);
    source_idx += NUM_CORES;

    int transparent_color = PreparePalette(palette[name.tile.palette_idx]);

    interp0->accum[0] = plane->tiles[(name.tile.tile_idx * TILE_WORDS) + (pattern_y ^ name.tile.flip_y)];

    if (transparent) {
      #pragma GCC unroll 8
      for (int i = 14; i >= 0; i -= 2) {
        int color = interp0->pop[1];
        if (color != transparent_color) {
          WriteDoublePixel(dest + i, LookupPalette(color));
        }
      }
    } else {
      #pragma GCC unroll 8
      for (int i = 14; i >= 0; i -= 2) {
        int color = interp0->pop[1];
        WriteDoublePixel(dest + i, LookupPalette(color));
      }
    }
    dest += 32;
  }
}

static void STRIPED_SECTION ScanOutTextMode(const PlaneContext* ctx, int core_num, int plane_idx, bool hires, int text_height) {
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
    ConfigureInterpolator(1, 0, 0);

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
    ConfigureInterpolator(1, 1, 1);

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

static void STRIPED_SECTION ScanOutSprites(const void* cc, int core_num) {
  const SpriteContext* ctx = cc;
  int y = ctx->y;

  ConfigureInterpolator(4 * NUM_CORES, 0, 3);

  for (int z = NUM_SPRITE_PRIORITIES - 1; z >= 0; --z) {
    for (int j = 0; j < count_of(g_active_sprites); ++j) {
      const ActiveSprite* active = &g_active_sprites[j];
      if (active->z != z)
        continue;
      if (active->y > y)
        continue;

      const uint32_t* src = g_sprite_layer.images + 16 * active->image_addr;
      if (active->flip_y) {
        src += (SPRITE_WIDTH/8) * (active->height - 1 - y - active->y);
      } else {
        src += (SPRITE_WIDTH/8) * (y - active->y);
      }

      int transparent = PreparePalette(g_sprite_palettes[active->palette_idx]);
      if (!active->transparent) {
        transparent = -1;
      }

      uint16_t* dest = ((uint16_t*) ctx->dest) + active->x;

      if (active->flip_x) {
        int x = (SPRITE_WIDTH - 2) + ((active->x & (NUM_CORES-1)) ^ core_num);
        for (; x >= 0; x -= 8) {
          interp0->accum[0] = (*src++) >> (core_num * 4);

          #pragma GCC unroll 4
          for (int i = 6; i >= 0; i -= 2) {
            int color = interp0->pop[1];
            if (color != transparent) {
              dest[x + i] = LookupPalette(color);
            }
          }
        }
      } else {
        int x = (active->x & (NUM_CORES-1)) ^ core_num;
        for (; x < SPRITE_WIDTH; x += 8) {
          interp0->accum[0] = (*src++) >> (core_num * 4);

          #pragma GCC unroll 4
          for (int i = 0; i < 8; i += 2) {
            int color = interp0->pop[1];
            if (color != transparent) {
              dest[x + i] = LookupPalette(color);
            }
          }
        }
      }
    }
  }
}

static void STRIPED_SECTION UpdateActiveSprites(int y) {
  for (int i = 0; i < NUM_ACTIVE_SPRITES; ++i) {
    ActiveSprite *active = &g_active_sprites[i];

    if (y >= active->y + active->height) {
      if (g_pending_sprite_idx < count_of(g_sprite_layer.sprites)) {
        const Sprite* sprite = &g_sprite_layer.sprites[g_pending_sprite_idx++];
        active->x = sprite->x;
        active->y = sprite->y;
        active->z = sprite->z;
        active->height = CalcSpriteHeight(sprite->height);
        active->image_addr = sprite->image_addr;
        active->palette_idx = sprite->palette_idx;
        active->flip_x = sprite->flip_x;
        active->flip_y = sprite->flip_y;
        active->transparent = sprite->transparent;

        if (active->x <= -SPRITE_WIDTH || active->x >= 320) {
          active->z = 0xFF;
        }
      } else {
        active->y = 240;
        active->height = 0;
        active->z = 0xFF;
      }
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

#pragma GCC pop_options

static void STRIPED_SECTION ScanOutPlanes(const void* cc, int core_num) {
  const PlaneContext* ctx = cc;
  bool transparent = false;
  for (int i = 0; i < NUM_PLANES; ++i) {
    // Don't scan out plane 0 if it is occluded by plane 1. This means we don't have to deal with the slowest
    // case of two 40 column text planes plus sprite layer.
    if (i == 0 && (g_plane_regs[1].plane_mode == PLANE_MODE_TEXT_40_30 || g_plane_regs[1].plane_mode == PLANE_MODE_TEXT_40_24))
      continue;

    const PlaneRegs* regs = &g_plane_regs[i];

    switch (regs->plane_mode) {
    case PLANE_MODE_TILE:
      ScanOutTileMode(ctx, core_num, i, transparent);
      transparent = true;
      break;
    case PLANE_MODE_TEXT_40_30:
      ScanOutTextMode(ctx, core_num, i, false, 8);
      transparent = true;
      break;
    case PLANE_MODE_TEXT_40_24:
      ScanOutTextMode(ctx, core_num, i, false, 10);
      transparent = true;
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

void STRIPED_SECTION ScanOutBeginDisplay() {
  for (int i = 0; i < NUM_PLANES; ++i) {
    g_plane_regs[i] = g_planes[i].regs;

    DoublePalettes(g_plane_palettes[i], g_planes[i].palettes, NUM_PALETTES);
  }
  
  DoublePalettes(g_sprite_palettes, g_sprite_layer.palettes, NUM_PALETTES);

  g_pending_sprite_idx = 0;
  for (int i = 0; i < NUM_ACTIVE_SPRITES; ++i) {
    g_active_sprites[i].y = 0;
    g_active_sprites[i].height = 0;
    g_active_sprites[i].z = 0xFF;
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
  g_sys80_regs.current_y = 0xFF;
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
  for (int y = 0; y < 16; ++y) {
    for (int x = 0; x < 16; ++x) {
      Sprite* sprite = &g_sprite_layer.sprites[i];
      sprite->height = 1;
      sprite->z = 1;
      sprite->x = x * 20 + y * 2 - 20;
      sprite->y = y * 26 + x - 20;
      ++i;
    }
  }

  for (; i < count_of(g_sprite_layer.sprites); ++i) {
    Sprite* sprite = &g_sprite_layer.sprites[i];
    sprite->y = -8;
    sprite->height = 0;
  }
}