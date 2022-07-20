#define INNER                                             \
  int color = interp0->pop[1];                            \
  if (PAIR) {                                             \
    if (TRANSPARENT) {                                    \
      dest[i] = dest[i] | (color << 4);                   \
    } else {                                              \
      dest[i] = color;                                    \
    }                                                     \
  } else {                                                \
    if (!TRANSPARENT || color != transparent) {           \
      dest[i] = LookupPalette(color);                     \
    }                                                     \
  }                                                       \


for (int c = 0; c < 41; c += NUM_CORES) {
  Name name = base_names[(begin_col + c) & (PLANE_WIDTH-1)];

  int transparent = 0;
  if (!PAIR) {
    transparent = PreparePalette(palettes[name.tile.palette_idx]);
  }

  int flip = name.tile.flip;
  interp0->accum[0] = base_tiles[flip][name.tile.tile_idx * TILE_WORDS];

  if (flip & 1) {
    #pragma GCC unroll 8
    for (int i = 0; i < 16; i += 2) {
      INNER
    }
  } else {
    #pragma GCC unroll 8
    for (int i = 14; i >= 0; i -= 2) {
      INNER
    }
  }
  dest += 32;
}

#undef INNER
