for (int c = 0; c < 41; c += NUM_CORES) {
  Name name = names[((row * PLANE_WIDTH) + ((begin_col + c) & (PLANE_WIDTH-1))) & (PLANE_WIDTH * PLANE_HEIGHT - 1)];

  int bitmap = plane->chars[name.text.char_idx * CHAR_BYTES + pattern_y];
  interp0->accum[0] = bitmap;

  uint8_t local_palette[] = {
    palette[name.text.bg_color],
    palette[name.text.fg_color],
  };

  if (HIRES) {
    #pragma GCC unroll 8
    for (int i = 7; i >= 0; --i) {
      dest[i] = local_palette[interp0->pop[1]];
    }
  } else {
    #pragma GCC unroll 8
    for (int i = 14; i >= 0; i -= 2) {
      dest[i] = local_palette[interp0->pop[1]];
    }
  }

  dest += 32;
}
