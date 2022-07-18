#if HIRES
  typedef uint8_t rgb_t;
#else
  typedef uint16_t rgb_t;
#endif

  if (HIRES) {
    ConfigureInterpolator(1, 0, 0);
  } else {
    ConfigureInterpolator(1, 1, 1);
  }

  for (int c = 0; c < 41; c += NUM_CORES) {
    Name name = source_base[(begin_col + c) & (PLANE_WIDTH-1)];

    int bitmap = plane->chars[name.text.char_idx * CHAR_BYTES + pattern_y];
    if (!HIRES) {
      bitmap *= 2;
    }
    interp0->accum[0] = bitmap;

    rgb_t local_palette[] = {
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
        int color = *(uint16_t*) (((uint8_t*) local_palette) + interp0->pop[1]);
        WriteDoublePixel(dest + i, color);
      }
    }

    dest += 32;
  }
