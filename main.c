#include <stdlib.h>
#include <stdio.h>
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/structs/syscfg.h"
#include "tusb.h"

#include "blit.h"
#include "pins.h"
#include "scan_out.h"
#include "section.h"
#include "sys80.h"
#include "video_dma.h"


void ScanMain() {
  tusb_init();

  InitVideoInterrupts();
  StartVideo();  

  for (;;) {
    tuh_task();
  }
}

int main() {
  stdio_init_all();

  gpio_init(LED_PIN);
  gpio_set_dir(LED_PIN, GPIO_OUT);

  // Give DMA controller bus priority over processors. For correct latency of read IO requests on the Z80
  // bus interface, these must not be delayed.
  //bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_R_BITS | BUSCTRL_BUS_PRIORITY_DMA_W_BITS;

  InitSys80();

  //InitScanOutTest(DISPLAY_MODE_LORES_16, timing->horz.display_pixels >> horz_shift, timing->vert.display_pixels >> vert_shift);

  const VideoTiming* timing = &g_timing1024_768;
  const int horz_shift = 1;
  const int vert_shift = 2;

  InitVideo(timing);
  SetVideoResolution(horz_shift, vert_shift);

  multicore_launch_core1(ScanMain);
  BlitMain();
}
