#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pwm.h"

#include "audio.h"

#include "pins.h"
#include "section.h"
#include "sys80.h"

#define AY_FREQ 1773400
#define CYCLES_PER_SAMPLE 16
#define SAMPLE_FREQ (AY_FREQ / CYCLES_PER_SAMPLE)
#define SAMPLE_PWM 1

static int g_pwm_slice, g_pwm_channel;
static int g_volume = 128;

void STRIPED_SECTION SampleISR() {
  if (pwm_get_irq_status_mask() & (1 << SAMPLE_PWM)) {
    pwm_clear_irq(SAMPLE_PWM);
    int level = GenerateAY(g_volume);
    pwm_set_chan_level(g_pwm_slice, g_pwm_channel, level);
  }
}

static void InitPWM() {
  // This is the PWM slice connected to the GPIO.
  gpio_set_function(PWM_PIN, GPIO_FUNC_PWM);

  g_pwm_slice = pwm_gpio_to_slice_num(PWM_PIN);
  g_pwm_channel = pwm_gpio_to_channel(PWM_PIN);

  pwm_config cfg = pwm_get_default_config();
  pwm_config_set_wrap(&cfg, 0xFF);
  pwm_config_set_phase_correct(&cfg, true);
  pwm_init(g_pwm_slice, &cfg, true);

  // This PWM slice is timed to assert a periodic interrupt at the sample frequency.
  cfg = pwm_get_default_config();
  pwm_config_set_wrap(&cfg, clock_get_hz(clk_sys) / SAMPLE_FREQ);
  pwm_init(SAMPLE_PWM, &cfg, false);

  pwm_set_irq_enabled(SAMPLE_PWM, true);
  irq_set_exclusive_handler(PWM_IRQ_WRAP, SampleISR);
  irq_set_priority(PWM_IRQ_WRAP, 0x00);
  irq_set_enabled(PWM_IRQ_WRAP, true);

  pwm_set_enabled(SAMPLE_PWM, true);
}

void InitAudio() {
  InitAY();
  InitPWM();
}

void ChangeVolume(int delta) {
  g_volume += delta;
  if (g_volume > 256) {
    g_volume = 256;
  }
  if (g_volume < 0) {
    g_volume = 0;
  }
}

