int res_mult = HIRES ? 1 : 2;

for (int i = count_of(g_sprite_buckets) - 1; i >= 0; --i) {
  for (const ActiveSprite* active = g_sprite_buckets[i]; active; active = active->next) {
    if (active->y > y)
      continue;

    int flip = active->flip;
    const uint32_t* src = active->src;
    if (flip & 2) {
      src += (SPRITE_WIDTH/8) * (active->height - 1 - y - active->y);
    } else {
      src += (SPRITE_WIDTH/8) * (y - active->y);
    }

    int transparent = PreparePalette(active->palette);

    uint8_t* dest = ctx->dest + active->x * res_mult;

    if (flip & 1) {
      int x = ((SPRITE_WIDTH - 8) + ((active->x & (NUM_CORES-1)) ^ core_num)) * res_mult;
      for (; x >= 0; x -= 8*res_mult) {
        interp0->accum[0] = (*src++) >> (core_num * 4);

        #pragma GCC unroll 4
        for (int i = 6*res_mult; i >= 0; i -= 2*res_mult) {
          int color = interp0->pop[1];
          if (color != transparent) {
            dest[x + i] = LookupPalette(color);
          }
        }
      }
    } else {
      int x = ((active->x & (NUM_CORES-1)) ^ core_num) * res_mult;
      for (; x < SPRITE_WIDTH*res_mult; x += 8*res_mult) {
        interp0->accum[0] = (*src++) >> (core_num * 4);

        #pragma GCC unroll 4
        for (int i = 0; i < 8*res_mult; i += 2*res_mult) {
          int color = interp0->pop[1];
          if (color != transparent) {
            dest[x + i] = LookupPalette(color);
          }
        }
      }
    }
  }
}
