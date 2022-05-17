#include <stdlib.h>
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/structs/syscfg.h"

#include "mode.h"
#include "pins.h"
#include "sys80.h"
#include "sys80_test.h"
#include "video.h"

void __not_in_flash_func(BlinkLoop)() {
  gpio_init(LED_PIN);
  gpio_set_dir(LED_PIN, GPIO_OUT);
  for (;;) {
      gpio_put(LED_PIN, 0);
      sleep_ms(250);
      gpio_put(LED_PIN, 1);
      sleep_ms(1000);
  }
}

void TestVideo() {

  const int bpp = 4;  // set to 2, 4 or 8

  VideoLineRenderer renderer = NULL;
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
    renderer);
}

int main() {
  stdio_init_all();

  // Give DMA controller bus priority over processors. For correct latency of read IO requests on the Z80
  // bus interface, these must not be delayed.
  bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_R_BITS | BUSCTRL_BUS_PRIORITY_DMA_W_BITS;

  InitSys80();
  TestVideo();
  BlinkLoop();

  //TestSys80();
}
