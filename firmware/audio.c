#include <math.h>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pwm.h"
#include "pico/multicore.h"

#include "audio.h"

#include "ay.h"
#include "pins.h"
#include "section.h"

#define AY_FREQ 1773400
#define CYCLES_PER_SAMPLE 16
#define SAMPLE_FREQ (AY_FREQ / CYCLES_PER_SAMPLE)
#define BASE_SAMPLE_PWM 1  // BASE_SAMPLE_PWM+1 is also used for core 1

static int g_pwm_slice, g_pwm_channel;
static int g_volume = 128;

static AYState g_ay_state[2];

static volatile int g_secondary_level;
static int g_dither_level;

void STRIPED_SECTION SampleISR() {
  int core_num = get_core_num();
  int sample_pwm = BASE_SAMPLE_PWM + core_num;

  if (pwm_get_irq_status_mask() & (1 << sample_pwm)) {
    pwm_clear_irq(sample_pwm);

    int level = GenerateAY(&g_ay_state[core_num], g_sys80_regs.ay[core_num]);

    if (core_num == 0) {
      level = (level + g_secondary_level) * g_volume + g_dither_level;
      g_dither_level = level & 0x1FFFF;
      pwm_set_chan_level(g_pwm_slice, g_pwm_channel, level >> 17);
    } else {
      g_secondary_level = level;
    }
  }
}

static void InitOutputPWM() {
  // This is the PWM slice connected to the GPIO.
  gpio_set_function(PWM_PIN, GPIO_FUNC_PWM);

  g_pwm_slice = pwm_gpio_to_slice_num(PWM_PIN);
  g_pwm_channel = pwm_gpio_to_channel(PWM_PIN);

  pwm_config cfg = pwm_get_default_config();
  pwm_config_set_wrap(&cfg, 0xFF);
  pwm_config_set_phase_correct(&cfg, true);
  pwm_init(g_pwm_slice, &cfg, true);
}

static void InitSamplePWM(int core_num) {
  // This PWM slice is timed to assert a periodic interrupt at the sample frequency.
  int sample_pwm = BASE_SAMPLE_PWM + core_num;

  pwm_config cfg = pwm_get_default_config();
  pwm_config_set_wrap(&cfg, clock_get_hz(clk_sys) / SAMPLE_FREQ);
  pwm_init(sample_pwm, &cfg, false);

  pwm_set_irq_enabled(sample_pwm, true);
  irq_set_exclusive_handler(PWM_IRQ_WRAP, SampleISR);
  irq_set_priority(PWM_IRQ_WRAP, 0x00);

  irq_set_enabled(PWM_IRQ_WRAP, true);

  pwm_set_enabled(sample_pwm, true);
}

void InitAudio() {
  int core_num = get_core_num();

  if (core_num == 0) {
    InitOutputPWM();
  }

  if (PICOVDP_ENABLE_DUAL_AUDIO || core_num == 0) {
    InitAY(&g_ay_state[core_num]);
    InitSamplePWM(core_num);
  }
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

