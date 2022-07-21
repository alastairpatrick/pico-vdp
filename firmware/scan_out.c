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

#define PAGE_SIZE 128

#define PLANE_WIDTH 64
#define PLANE_HEIGHT 64
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

typedef uint8_t Palette[16];

typedef struct {
  union {
    uint8_t bytes[65536];
    uint32_t tiles[65536 / sizeof(uint32_t)];
  };

  const Name* mid_names;
  const Name* edge_names;
  const Palette* palettes;
  const uint8_t* chars;

  int mid_top, mid_bottom;
  int mid_x, mid_y;
  int top_x, bottom_x;
} Plane;

typedef struct {
  uint16_t z: SPRITE_PRIORITY_BITS;
  int16_t y: 10;

  uint16_t flip: 2;
  bool opaque: 1;
  int16_t x: 11;
  
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

  uint8_t height;
  uint8_t flip: 2;

  const uint32_t* src;

  uint8_t* palette;
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

typedef uint8_t Palette[PALETTE_SIZE];

static Plane g_planes[NUM_PLANES];
static Palette g_plane_palettes[NUM_PLANES][NUM_PALETTES];

static SpriteLayer g_sprite_layer;
static Palette g_sprite_palettes[NUM_PALETTES];

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
  PLANE_REGION_MID,
  PLANE_REGION_BOTTOM,
} PlaneRegion;

static PlaneRegion STRIPED_SECTION GetPlaneRegion(const Plane* plane, int y) {
  if (y < plane->mid_top) {
    return PLANE_REGION_TOP;
  } else if (y >= plane->mid_bottom) {
    return PLANE_REGION_BOTTOM;
  } else {
    return PLANE_REGION_MID;
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

static int STRIPED_SECTION PreparePalette(const uint8_t* palette) {
  return interp0->base[1] = (uint32_t) palette;
}

static int STRIPED_SECTION LookupPalette(int color) {
  return *(uint8_t*) color;
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

  int x = 0;
  int y;
  const Name* names;

  PlaneRegion region = GetPlaneRegion(plane, ctx->y);
  if (region == PLANE_REGION_MID) {
    y = (ctx->y + plane->mid_y) & (PLANE_HEIGHT*TILE_SIZE-1);
    names = plane->mid_names;
    x = plane->mid_x & (PLANE_WIDTH*TILE_SIZE-1);
  } else {
    if (region == PLANE_REGION_TOP) {
      y = ctx->y;
      x = plane->top_x;
    } else {
      y = ctx->y - plane->mid_bottom;
      x = plane->bottom_x;
    }

    names = plane->edge_names;
  }

  int row = y >> TILE_SIZE_BITS;
  int pattern_y = y & (TILE_SIZE-1);

  const uint32_t* base_tiles[4] = {  // indexed by name.tile.flip_y
    plane->tiles + pattern_y,
    plane->tiles + pattern_y,
    plane->tiles + TILE_SIZE-1 - pattern_y,
    plane->tiles + TILE_SIZE-1 - pattern_y,
  };

  int begin_col = (x >> TILE_SIZE_BITS) + core_num;

  static_assert(TILE_SIZE * 2 == 16);
  uint8_t* dest = ctx->dest - (x & (TILE_SIZE-1)) * 2 + (core_num * TILE_SIZE * 2);

  const Palette* palettes = g_plane_palettes[plane_idx];
  
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
  int mid_x = plane->mid_x & (PLANE_WIDTH-1);
  int mid_y = plane->mid_y & (PLANE_HEIGHT-1);

  int y = ctx->y + mid_y * text_height;
  int32_t pattern_y = y % text_height;
  int row = y / text_height;

  const Name* names = plane->mid_names;
  int begin_col = mid_x + core_num;

  const uint8_t* palette = g_plane_palettes[plane_idx][0];

  static_assert(TEXT_WIDTH * 2 == 16);
  uint8_t* dest = ctx->dest + (core_num * TEXT_WIDTH * 2);
  if (hires) {
    dest += plane_idx * TEXT_WIDTH;
  }

  ConfigureInterpolator(1, 0, 0);

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
  int plane_flags;
} SpriteContext;

static void STRIPED_SECTION ScanOutSprites(const void* cc, int core_num) {
  const SpriteContext* ctx = cc;
  int y = ctx->y;
  int plane_flags = ctx->plane_flags;

  ConfigureInterpolator(4 * NUM_CORES, 0, 3);

  if ((plane_flags & (PLANE_FLAG_PAIR_EN | PLANE_FLAG_TEXT_EN)) == (PLANE_FLAG_PAIR_EN | PLANE_FLAG_TEXT_EN)) {
    #define HIRES 1
    #include "scan_sprite_templ.h"
    #undef HIRES
  } else {
    #define HIRES 0
    #include "scan_sprite_templ.h"
    #undef HIRES
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
        active->src = g_sprite_layer.images + 16 * sprite->image_addr;
        active->flip = sprite->flip;
        active->palette = g_sprite_palettes[sprite->palette_idx];
        active->transparent = sprite->opaque ? -1 : (int) active->palette;

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

static void STRIPED_SECTION CopyPalettes(Palette* dest, const Palette* source, int num) {
  for (int i = 0; i < num; ++i) {
    for (int j = 0; j < PALETTE_SIZE; ++j) {
      dest[i][j] = source[i][j];
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
    Plane* plane = &g_planes[i];
    volatile PlaneRegs* regs = &g_sys80_regs.plane_regs[i];
    
    /*if (t % 3 == 0) {
      ++plane->mid_x;
    }
    if (t % 5 == 0) {
      ++plane->mid_y;
      --plane->top_x;
      --plane->bottom_x;
    }

    plane->mid_top = 16;
    plane->mid_bottom = 240-16;*/

    plane->mid_names = (const Name*) (plane->bytes + PAGE_SIZE * regs->mid_page);
    plane->edge_names = (const Name*) (plane->bytes + PAGE_SIZE * regs->edge_page);
    plane->palettes = (const Palette*) (plane->bytes + PAGE_SIZE * regs->palette_page);
    plane->chars = plane->bytes + PAGE_SIZE * regs->chars_page;

    plane->mid_top = g_sys80_regs.plane_regs[i].mid_top;
    plane->mid_bottom = g_sys80_regs.plane_regs[i].mid_bottom;
    plane->mid_x = Unswizzle16BitSys80Reg(regs->mid_x);
    plane->mid_y = Unswizzle16BitSys80Reg(regs->mid_y);
    plane->top_x = Unswizzle16BitSys80Reg(regs->top_x);
    plane->bottom_x = Unswizzle16BitSys80Reg(regs->bottom_x);

    CopyPalettes(g_plane_palettes[i], plane->palettes, NUM_PALETTES);

    UpdateActiveSprites(0);
  }
  
  CopyPalettes(g_sprite_palettes, g_sprite_layer.palettes, NUM_PALETTES);

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

void STRIPED_SECTION ScanOutSpritesParallel(uint8_t* dest, int y, int plane_flags) {
  SpriteContext ctx = {
    .parallel.entry = ScanOutSprites,
    .plane_flags = plane_flags,
    .dest = dest,
    .y = y,
  };

  Parallel(&ctx);
}

void STRIPED_SECTION ScanOutLine(uint8_t* dest, int y) {
  g_sys80_regs.current_y = y;

  int plane_flags = g_sys80_regs.plane_flags;
  int sprite_flags = g_sys80_regs.sprite_flags;

  bool hires_video = (plane_flags & (PLANE_FLAG_PAIR_EN | PLANE_FLAG_TEXT_EN)) == (PLANE_FLAG_PAIR_EN | PLANE_FLAG_TEXT_EN);
  EnableHires(hires_video);

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
    ScanOutSpritesParallel(dest, y, plane_flags);
  }

  if (plane_flags & PLANE_FLAG_PLANE_1_EN) {
    ScanOutPlaneParallel(dest, y, 1, plane_flags);
  }

  if (sprite_en && sprite_pri) {
    ScanOutSpritesParallel(dest, y, plane_flags);
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
  g_sys80_regs.plane_regs[1].mid_top = 16;
  g_sys80_regs.plane_regs[1].mid_bottom = DISPLAY_HEIGHT - 16;

  for (int i = 0; i < count_of(g_planes->bytes); ++i) {
    g_planes[0].bytes[i] = rand();
    g_planes[1].bytes[i] = rand();
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