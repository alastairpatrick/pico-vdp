#include "pico/stdlib.h"
#include "hardware/adc.h"

#include "supply.h"

#include "common.h"

#define SUPPLY_ALARM_THRESHOLD ((int) (0xFFF * 4.5 * 10. / (10. + 5.6) / 3.3))
#define SUPPLY_ALARM_DURATION_US (1000000 / 10)

static int64_t g_last_supply_alarm_time;

void InitSupplyMonitor() {
  if (PICOVDP_MONITOR_SUPPLY) {
    adc_select_input(SUPPLY_PIN);
    hw_set_bits(&adc_hw->cs, ADC_CS_START_ONCE_BITS);
    g_last_supply_alarm_time = -SUPPLY_ALARM_DURATION_US;
  }
}

void UpdateSupplyMonitor() {
  if (PICOVDP_MONITOR_SUPPLY && (adc_hw->cs & ADC_CS_READY_BITS)) {
    int64_t now = time_us_64();

    int voltage = adc_hw->result;
    if (voltage < SUPPLY_ALARM_THRESHOLD) {
      g_last_supply_alarm_time = now;
    }
    gpio_put(LED_PIN, (now - g_last_supply_alarm_time) < SUPPLY_ALARM_DURATION_US);

    hw_set_bits(&adc_hw->cs, ADC_CS_START_ONCE_BITS);
  }
}
