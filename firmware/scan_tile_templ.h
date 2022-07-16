for (int c = 0; c < 41; c += NUM_CORES) {
  Name name = GetName(plane, source_idx);
  source_idx += NUM_CORES;

  int transparent_color = PreparePalette(palette[name.tile.palette_idx]);

  int flip = name.tile.flip;
  interp0->accum[0] = base_tiles[flip][name.tile.tile_idx * TILE_WORDS];

  if (flip & 1) {
    #pragma GCC unroll 8
    for (int i = 0; i < 16; i += 2) {
      int color = interp0->pop[1];
      if (!TRANSPARENT || color != transparent_color) {
        WriteDoublePixel(dest + i, LookupPalette(color));
      }
    }
  } else {
    #pragma GCC unroll 8
    for (int i = 14; i >= 0; i -= 2) {
      int color = interp0->pop[1];
      if (!TRANSPARENT || color != transparent_color) {
        WriteDoublePixel(dest + i, LookupPalette(color));
      }
    }
  }
  dest += 32;
}
