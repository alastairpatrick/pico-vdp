#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include "pico/stdlib.h"

#include "video.h"

uint32_t g_vram[128 * 1024 / 4];

uint8_t g_palette[16] = {
  0b00000100,
  0b00001000,
  0b00001100,
  0b00010000,
  0b00010100,
  0b00011100,

  0b00000000,
  0b00000001,
  0b00000010,
  0b00000011,

  0b00100000,
  0b01000000,
  0b01100000,
  0b10000000,
  0b10100000,
  0b11100000,
};

void InitVRAMTest(int bpp) {
  uint32_t* dest = g_vram;
  int rgb = 0;

  switch (bpp) {
    case 2:
      while (dest < g_vram + count_of(g_vram)) {
        *dest++ = 0b11100100111001001110010011100100;
      }
      break;
    case 4:
      while (dest < g_vram + count_of(g_vram)) {
        *dest++ = 0x76543210;
        *dest++ = 0xFEDCBA98;
      }
      break;
    case 8:
      while (dest < g_vram + count_of(g_vram)) {
        *dest++ = ((rgb+3) << 24) | ((rgb+2) << 16) | ((rgb+1) << 8 | (rgb));
        rgb += 4;
      }
      break;
    default:
      assert(false);
  }
}

void __not_in_flash_func(RenderLine4)(uint32_t* dest, int line, int width) {
  uint32_t* source = g_vram + line * width / sizeof(g_vram[0]) / 4;

  for (int x = 0; x < width/16; ++x) {
    uint32_t indices16 = *source++;

    *dest++ = g_palette[indices16 & 0x3]
            | (g_palette[(indices16 >> 2) & 0x3] << 8)
            | (g_palette[(indices16 >> 4) & 0x3] << 16)
            | (g_palette[(indices16 >> 6) & 0x3] << 24);

    *dest++ = g_palette[(indices16 >> 8) & 0x3]
            | (g_palette[(indices16 >> 10) & 0x3] << 8)
            | (g_palette[(indices16 >> 12) & 0x3] << 16)
            | (g_palette[(indices16 >> 14) & 0x3] << 24);

    *dest++ = g_palette[(indices16 >> 16) & 0x3]
            | (g_palette[(indices16 >> 18) & 0x3] << 8)
            | (g_palette[(indices16 >> 20) & 0x3] << 16)
            | (g_palette[(indices16 >> 22) & 0x3] << 24);

    *dest++ = g_palette[(indices16 >> 24) & 0x3]
            | (g_palette[(indices16 >> 26) & 0x3] << 8)
            | (g_palette[(indices16 >> 28) & 0x3] << 16)
            | (g_palette[(indices16 >> 30) & 0x3] << 24);
  }
}

void __not_in_flash_func(RenderLine16)(uint32_t* dest, int line, int width) {
  uint32_t* source = g_vram + line * width / sizeof(g_vram[0]) / 2;

  for (int x = 0; x < width/8; ++x) {
    uint32_t indices8 = *source++;

    *dest++ = g_palette[indices8 & 0xF]
            | (g_palette[(indices8 >> 4) & 0xF] << 8)
            | (g_palette[(indices8 >> 8) & 0xF] << 16)
            | (g_palette[(indices8 >> 12) & 0xF] << 24);

    *dest++ = g_palette[(indices8 >> 16) & 0xF]
            | (g_palette[(indices8 >> 20) & 0xF] << 8)
            | (g_palette[(indices8 >> 24) & 0xF] << 16)
            | (g_palette[(indices8 >> 28) & 0xF] << 24);
  }
}

void __not_in_flash_func(RenderLine256)(uint32_t* dest, int line, int width) {
  uint32_t* source = g_vram + line * width / sizeof(g_vram[0]);

  for (int x = 0; x < width/4; ++x) {
    *dest++ = *source++;
  }
}
