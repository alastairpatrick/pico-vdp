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

// -fno-strict-aliasing because the usual memcpy() approach to type punning does not work; the
// compiler generates a call to memcpy() rather than inlining even when just copying a single uint16_t.
#pragma GCC optimize("no-strict-aliasing")

#define AWFUL_MAGENTA 0xC7

#define PLANE_WIDTH 64
#define PLANE_HEIGHT 64

#define FIXED_WIDTH 40
#define FIXED_HEIGHT 30

#define NUM_PALETTES 8
#define PALETTE_SIZE 16
#define NUM_PLANES 2

#define TILE_SIZE_BITS 3
#define TILE_SIZE (1 << TILE_SIZE_BITS)
#define TILE_WORDS 8

#define TEXT_WIDTH 8
#define CHAR_BYTES 16

#define SPRITE_WIDTH 16
#define NUM_ACTIVE_SPRITES 16
#define SPRITE_PRIORITY_BITS 4
#define NUM_SPRITE_PRIORITIES (1 << SPRITE_PRIORITY_BITS)

typedef struct {
  union {
    struct {
      uint16_t tile_idx   : 11;
      uint8_t palette_idx : 3;
      uint8_t flip        : 2;
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
      Name scroll_names[PLANE_WIDTH * PLANE_HEIGHT];  // 8192 bytes
      uint8_t palettes[NUM_PALETTES][PALETTE_SIZE];   // 128 bytes
      uint8_t pad1[128];                              // 128 bytes
      Name fixed_names[FIXED_WIDTH * FIXED_HEIGHT];   // 2400 bytes
      uint8_t pad2[1440];                             // 1440 bytes
      uint8_t chars[256 * CHAR_BYTES];                // 4096 bytes
    };
    uint8_t bytes[65536];
    uint32_t tiles[65536 / sizeof(uint32_t)];
  };

  int window_x, window_y;
  int scroll_top, scroll_bottom;
} Plane;

static_assert(offsetof(Plane, scroll_names) == 0x0000);
static_assert(offsetof(Plane, palettes) == 0x2000);
static_assert(offsetof(Plane, fixed_names) == 0x2100);
static_assert(offsetof(Plane, chars) == 0x3000);

typedef struct {
  uint16_t z: SPRITE_PRIORITY_BITS;
  int16_t y: 10;

  uint16_t flip: 2;
  bool opaque: 1;
  int16_t x: 10;
  
  uint16_t image_addr: 10;
  uint8_t height: 3;  // 8 * (height + 1)
  uint8_t palette_idx: 3;
} Sprite;

static_assert(sizeof(Sprite) == 6);

typedef struct ActiveSprite {
  struct ActiveSprite* next;

  int16_t x;
  int16_t y;

  uint8_t z;

  uint16_t image_addr;

  uint8_t height;
  uint8_t flip: 2;

  int half_palette;
  int transparent;
} ActiveSprite;

static ActiveSprite g_active_sprites[NUM_ACTIVE_SPRITES];
static ActiveSprite *g_sprite_buckets[NUM_SPRITE_PRIORITIES];
static int g_pending_sprite_idx;

typedef struct {
  union {
    struct {
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

static int STRIPED_SECTION CalcSpriteHeight(int h) {
  return 8 * (h + 1);
}


typedef enum {
  PLANE_REGION_TOP,
  PLANE_REGION_SCROLL,
  PLANE_REGION_BOTTOM,
} PlaneRegion;

static PlaneRegion STRIPED_SECTION GetPlaneRegion(const Plane* plane, int y) {
  if (y < plane->scroll_top) {
    return PLANE_REGION_TOP;
  } else if (y >= plane->scroll_bottom) {
    return PLANE_REGION_BOTTOM;
  } else {
    return PLANE_REGION_SCROLL;
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
  return plane->scroll_names[idx & (count_of(plane->scroll_names) - 1)];
}

static void STRIPED_SECTION WriteDoublePixel(uint8_t* dest, int rgb) {
  *(uint16_t*) dest = rgb;
}

static int STRIPED_SECTION HalfPalette(const uint16_t* palette) {
  return ((int) palette) >> 1;
}

static int STRIPED_SECTION LookupPalette(int color) {
  int rgb;
  asm ("LDRH %0, [%1, %1]" : "=l" (rgb) : "l" (color));
  return rgb;
}


typedef struct {
  ParallelContext parallel;
  uint8_t* dest;
  uint8_t rgb;
} ClearContext;

static void STRIPED_SECTION ScanOutClear(const void* cc, int core_num) {
  const ClearContext* ctx = cc;

  uint32_t rgb = ctx->rgb;
  rgb = rgb | (rgb << 8);
  rgb = rgb | (rgb << 16);

  uint8_t* dest = ctx->dest;

  for (int x = core_num * 4; x < HIRES_DISPLAY_WIDTH; x += 4 * NUM_CORES) {
    *((uint32_t*) (dest + x)) = rgb; 
  }
}


typedef struct {
  ParallelContext parallel;
  int plane_idx;
  int plane_flags;
  int sprite_flags;
  uint8_t* dest;
  int y;
} PlaneContext;

static void STRIPED_SECTION ScanOutTileMode(const PlaneContext* ctx, int core_num) {
  int plane_idx = ctx->plane_idx;
  const Plane* plane = &g_planes[plane_idx];

  int window_x = 0;
  int y;
  const Name* base_names;

  PlaneRegion region = GetPlaneRegion(plane, y);
  if (region == PLANE_REGION_SCROLL) {
    y = (ctx->y + plane->window_y) & (PLANE_HEIGHT*TILE_SIZE-1);
    base_names = plane->scroll_names + (y >> TILE_SIZE_BITS) * PLANE_WIDTH;
    window_x = plane->window_x & (PLANE_WIDTH*TILE_SIZE-1);
  } else {
    if (region == PLANE_REGION_TOP) {
      y = ctx->y;
    } else {
      y = ctx->y - plane->scroll_bottom;
    }

    base_names = plane->fixed_names + (y >> TILE_SIZE_BITS) * FIXED_WIDTH;
  }

  int pattern_y = y & (TILE_SIZE-1);
  const uint32_t* base_tiles[4] = {  // indexed by name.tile.flip_y
    plane->tiles + pattern_y,
    plane->tiles + pattern_y,
    plane->tiles + TILE_SIZE-1 - pattern_y,
    plane->tiles + TILE_SIZE-1 - pattern_y,
  };

  int begin_col = (window_x >> TILE_SIZE_BITS) + core_num;

  static_assert(TILE_SIZE * 2 == 16);
  uint8_t* dest = ctx->dest - (window_x & (TILE_SIZE-1)) * 2 + (core_num * TILE_SIZE * 2);

  const Palette16* palette = g_plane_palettes[plane_idx];
  
  ConfigureInterpolator(4, 0, 3);

  if (ctx->plane_flags & PLANE_FLAG_PAIR_EN) {
    #define PAIR 1
    if (plane_idx == 1) {
      #define TRANSPARENT 1
      #include "scan_tile_templ.h"
      #undef TRANSPARENT
    } else {
      #define TRANSPARENT 0
      #include "scan_tile_templ.h"
      #undef TRANSPARENT
    }
    #undef PAIR
  } else {
    #define PAIR 0
    if (plane_idx == 1) {
      #define TRANSPARENT 1
      #include "scan_tile_templ.h"
      #undef TRANSPARENT
    } else {
      #define TRANSPARENT 0
      #include "scan_tile_templ.h"
      #undef TRANSPARENT
    }
    #undef PAIR
  }
}

static void STRIPED_SECTION ScanOutTextMode(const PlaneContext* ctx, int core_num, bool hires, int text_height) {
  int plane_idx = ctx->plane_idx;
  const Plane* plane = &g_planes[plane_idx];
  int window_x = plane->window_x & (PLANE_WIDTH-1);
  int window_y = plane->window_y & (PLANE_HEIGHT-1);

  int y = ctx->y + window_y * text_height;
  int32_t pattern_y = y % text_height;
  int char_y = y / text_height;

  const Name* source_base = plane->scroll_names + (char_y * PLANE_WIDTH);
  int begin_col = window_x + core_num;

  const uint16_t* palette = g_plane_palettes[plane_idx][0];

  static_assert(TEXT_WIDTH * 2 == 16);
  uint8_t* dest = ctx->dest + (core_num * TEXT_WIDTH * 2);
  if (hires) {
    dest += plane_idx * TEXT_WIDTH;
  }

  if (hires) {
    #define HIRES 1
    #include "scan_text_templ.h"
    #undef HIRES
  } else {
    #define HIRES 0
    #include "scan_text_templ.h"
    #undef HIRES  
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

  for (int i = count_of(g_sprite_buckets) - 1; i >= 0; --i) {
    for (const ActiveSprite* active = g_sprite_buckets[i]; active; active = active->next) {
      if (active->y > y)
        continue;

      int flip = active->flip;
      const uint32_t* src = g_sprite_layer.images + 16 * active->image_addr;
      if (flip & 2) {
        src += (SPRITE_WIDTH/8) * (active->height - 1 - y - active->y);
      } else {
        src += (SPRITE_WIDTH/8) * (y - active->y);
      }

      interp0->base[1] = active->half_palette;
      int transparent = active->transparent;

      uint16_t* dest = ((uint16_t*) ctx->dest) + active->x;

      if (flip & 1) {
        int x = (SPRITE_WIDTH - 8) + ((active->x & (NUM_CORES-1)) ^ core_num);
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
  #pragma GCC unroll 16
  for (int i = 0; i < count_of(g_sprite_buckets); ++i) {
    g_sprite_buckets[i] = NULL;
  }

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
        active->flip = sprite->flip;
        active->half_palette = HalfPalette(g_sprite_palettes[sprite->palette_idx]);
        active->transparent = sprite->opaque ? -1 : active->half_palette;

        if (active->x <= -SPRITE_WIDTH || active->x >= LORES_DISPLAY_WIDTH) {
          active->y = DISPLAY_HEIGHT;
          active->height = 0;
        }
      } else {
        active->y = DISPLAY_HEIGHT;
        active->height = 0;
      }
    }

    active->next = g_sprite_buckets[active->z];
    g_sprite_buckets[active->z] = active;
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

static void STRIPED_SECTION ScanOutPlane(const void* cc, int core_num) {
  const PlaneContext* ctx = cc;
  int plane_idx = ctx->plane_idx;
  int plane_flags = ctx->plane_flags;

  if (plane_flags & PLANE_FLAG_TEXT_EN) {
    int text_height = plane_flags & PLANE_FLAG_TEXT_ROWS ? 10 : 8;

    if (plane_flags & PLANE_FLAG_PAIR_EN) {
      ScanOutTextMode(ctx, core_num, true, text_height);
    } else if (plane_idx == 0) {
      ScanOutTextMode(ctx, core_num, false, text_height);
    } else {
      ScanOutTileMode(ctx, core_num);
    }
  } else {
    ScanOutTileMode(ctx, core_num);
  }
}

void STRIPED_SECTION ScanOutBeginDisplay() {
  static int t;
  ++t;
  for (int i = 0; i < NUM_PLANES; ++i) {
    /*if (t % 3 == 0) {
      ++g_planes[i].window_x;
    }
    if (t % 5 == 0) {
      ++g_planes[i].window_y;
    }

    g_planes[i].scroll_top = 16;
    g_planes[i].scroll_bottom = 240-16;*/

    g_planes[i].window_x = Unswizzle16BitSys80Reg(g_sys80_regs.plane_regs[i].window_x);
    g_planes[i].window_y = Unswizzle16BitSys80Reg(g_sys80_regs.plane_regs[i].window_y);
    g_planes[i].scroll_top = g_sys80_regs.plane_regs[i].scroll_top;
    g_planes[i].scroll_bottom = g_sys80_regs.plane_regs[i].scroll_bottom;

    DoublePalettes(g_plane_palettes[i], g_planes[i].palettes, NUM_PALETTES);

    UpdateActiveSprites(0);
  }
  
  DoublePalettes(g_sprite_palettes, g_sprite_layer.palettes, NUM_PALETTES);

  g_pending_sprite_idx = 0;
  for (int i = 0; i < NUM_ACTIVE_SPRITES; ++i) {
    g_active_sprites[i].y = 0;
    g_active_sprites[i].height = 0;
    g_active_sprites[i].z = 0xFF;
  }
}

void STRIPED_SECTION ScanOutPlaneParallel(uint8_t* dest, int y, int plane_idx, int plane_flags) {
  PlaneContext ctx = {
    .parallel.entry = ScanOutPlane,
    .plane_idx = plane_idx,
    .plane_flags = plane_flags,
    .dest = dest,
    .y = y,
  };

  Parallel(&ctx);
}

void STRIPED_SECTION ScanOutSpritesParallel(uint8_t* dest, int y) {
  SpriteContext ctx = {
    .parallel.entry = ScanOutSprites,
    .dest = dest,
    .y = y,
  };

  Parallel(&ctx);
}

void STRIPED_SECTION ScanOutLine(uint8_t* dest, int y) {
  g_sys80_regs.current_y = y;

  int plane_flags = g_sys80_regs.plane_flags;
  int sprite_flags = g_sys80_regs.sprite_flags;

  // If either plane is disabled, initialize to solid black.
  if ((plane_flags & (PLANE_FLAG_PLANE_0_EN | PLANE_FLAG_PLANE_1_EN)) != (PLANE_FLAG_PLANE_0_EN | PLANE_FLAG_PLANE_1_EN)) {
    ClearContext clear_ctx = {
      .parallel.entry = ScanOutClear,
      .dest = dest,
      .rgb = 0,
    };

    Parallel(&clear_ctx);
  }

  if (plane_flags & PLANE_FLAG_PLANE_0_EN) {
    ScanOutPlaneParallel(dest, y, 0, plane_flags);
  }

  bool sprite_pri;
  if (plane_flags & PLANE_FLAG_PAIR_EN) {
    sprite_pri = true;
  } else {
    PlaneRegion region = GetPlaneRegion(&g_planes[1], y);
    if (region == PLANE_REGION_TOP) {
      sprite_pri = sprite_flags & SPRITE_FLAG_PRI_TOP;
    } else if (region == PLANE_REGION_BOTTOM) {
      sprite_pri = sprite_flags & SPRITE_FLAG_PRI_BOTTOM;
    } else {
      sprite_pri = sprite_flags & SPRITE_FLAG_PRI_SCROLL;
    }
  }

  bool sprite_en = sprite_flags & SPRITE_FLAG_EN;
  if (sprite_en && !sprite_pri) {
    ScanOutSpritesParallel(dest, y);
  }

  if (plane_flags & PLANE_FLAG_PLANE_1_EN) {
    ScanOutPlaneParallel(dest, y, 1, plane_flags);
  }

  if (sprite_en && sprite_pri) {
    ScanOutSpritesParallel(dest, y);
  }

  UpdateActiveSprites(y + 1);
}

void STRIPED_SECTION ScanOutEndDisplay() {
  g_sys80_regs.current_y = 0xFF;
}

// ===================== UNOPTIMIZED CODE BELOW =====================
//#pragma GCC optimize("Og")

void InitScanOut() {
  g_sys80_regs.plane_flags = PLANE_FLAG_PLANE_0_EN | PLANE_FLAG_PLANE_1_EN | PLANE_FLAG_PAIR_EN;
  g_sys80_regs.sprite_flags = SPRITE_FLAG_EN | SPRITE_FLAG_PRI_SCROLL;
  g_sys80_regs.plane_regs[1].scroll_top = 16;
  g_sys80_regs.plane_regs[1].scroll_bottom = DISPLAY_HEIGHT - 16;

  for (int i = 0; i < count_of(g_planes->bytes); ++i) {
    g_planes[0].bytes[i] = rand();
    g_planes[1].bytes[i] = rand();
  }

  for (int i = 0; i < 16; ++i) {
    g_planes[0].palettes[0][i] = g_planes[1].palettes[0][i] = rand();
  }

  for (int i = 0; i < count_of(g_planes->scroll_names); ++i) {
    for (int j = 0; j < 2; ++j) {
      Name name = {
        .tile.tile_idx = i,
      };
      g_planes[j].scroll_names[i] = name;
    }
  }

  memset(g_sprite_layer.bytes, 0x11, sizeof(g_sprite_layer.bytes));

  int i = 0;
  for (int y = 0; y < 16; ++y) {
    for (int x = 0; x < 16; ++x) {
      Sprite* sprite = &g_sprite_layer.sprites[i];
      sprite->height = 1;
      sprite->z = 1;
      sprite->x = x * 20;// + y * 2 - 20;
      sprite->y = y * 26;// + x - 20;
      sprite->flip = 0b11;
      ++i;
    }
  }

  for (; i < count_of(g_sprite_layer.sprites); ++i) {
    Sprite* sprite = &g_sprite_layer.sprites[i];
    sprite->y = -8;
    sprite->height = 0;
  }
}