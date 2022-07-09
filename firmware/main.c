#include <stdlib.h>
#include <stdio.h>
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/structs/syscfg.h"
#include "tusb.h"

#include "audio.h"
#include "hid.h"
#include "parallel.h"
#include "perf.h"
#include "pins.h"
#include "scan_out.h"
#include "section.h"
#include "supply.h"
#include "sys80.h"
#include "video_dma.h"

void InitLED() {
  gpio_init(LED_PIN);
  gpio_set_dir(LED_PIN, GPIO_OUT);
}

int main() {
  stdio_init_all();
  adc_init();
  tusb_init();

  InitParallel();
  InitPerf();

  InitAudio();
  InitLED();
  InitScanOut();
  InitSupplyMonitor();
  
  InitVideo(&g_timing640_480);
  SetVideoResolution(0, 1);
  InitVideoInterrupts();
  StartVideo();  

  InitSys80();

  for (;;) {
    UpdateKeyboard();
    UpdateSupplyMonitor();
    tuh_task();
  }
}
