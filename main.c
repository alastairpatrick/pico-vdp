#include <stdlib.h>
#include <stdio.h>
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/structs/syscfg.h"
#include "tusb.h"

#include "audio.h"
#include "hid.h"
#include "blit.h"
#include "perf.h"
#include "pins.h"
#include "scan_out.h"
#include "section.h"
#include "sys80.h"
#include "video_dma.h"


void ScanMain() {
  InitPerf();
  tusb_init();

#if PICOVDP_ENABLE_AUDIO
  InitAudio();
#endif

  InitVideoInterrupts();
  StartVideo();  

  for (;;) {
    UpdateKeyboardLEDs();
    tuh_task();
  }
}

void InitLED() {
  gpio_init(LED_PIN);
  gpio_set_dir(LED_PIN, GPIO_OUT);
}

int main() {
  stdio_init_all();
  InitPerf();
  InitLED();
  
  InitVideo(&g_timing1024_768);
  SetVideoResolution(1, 2);

  InitSys80();

  multicore_launch_core1(ScanMain);
  BlitMain();
}
