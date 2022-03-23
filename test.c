#include <stdlib.h>
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/structs/bus_ctrl.h"

#include "mode.h"
#include "video.h"

int main() {
  stdio_init_all();

  // High bus priority for processor 0.
  //hw_set_bits(&bus_ctrl_hw->priority, 1);

  const int bpp = 4;  // set to 2, 4 or 8

  LineRenderer renderer = NULL;
  switch (bpp) {
    case 2:
      renderer = RenderLine4;
      break;
    case 4:
      renderer = RenderLine16;
      break;
    case 8:
      renderer = RenderLine256;
      break;
    default:
      assert(false);
  }

  InitVRAMTest(bpp);

  // If it flickers or doesn't sync, it might mean the CPU can't keep up. Try lower res.
  // 256 color mode is actually the least expensive. 16 color mode is the most expensive and
  // most likely to flicker at high resolution. 4 color mode is slighely less expensive
  // than 16 color.
  InitVideo(
    &g_timing640_480,  // g_timing640_480, g_timing800_600 or g_timing1024_768
    2,                 // divisor for horizontal resolution 1, 2 or 4
    2,                 // divisor for vertical resolution 2 or 4
    0, renderer);

  for (;;) {
    tight_loop_contents();
  }
}
