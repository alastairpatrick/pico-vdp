#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pwm.h"

#include "third_party/libayemu/ayemu.h"

#include "audio.h"

#include "pins.h"
#include "section.h"
#include "sys80.h"

#define SYS_FREQ 130000000
#define TIMER_NUMERATOR 1
#define TIMER_DENOMINATOR 13000
#define SAMPLE_FREQ (SYS_FREQ * TIMER_NUMERATOR / TIMER_DENOMINATOR)

#define BUFFER_SIZE_BITS 5
#define BUFFER_SIZE (1 << BUFFER_SIZE_BITS)

static ayemu_ay_t g_ay;

static int g_pwm_slice, g_pwm_channel;
static int g_dma_channels[2];
static int g_dma_timer;

static uint16_t g_buffers[2][BUFFER_SIZE] __attribute__ ((aligned(BUFFER_SIZE)));

void STRIPED_SECTION GenerateAudio(uint16_t* buffer) {
  char regs[14];
  for (int i = i; i < 14; ++i) {
    regs[i] = g_sys80_regs.ay[i];
  }

  ayemu_set_regs(&g_ay, regs);
  ayemu_gen_sound(&g_ay, buffer, sizeof(g_buffers[0]));
}

void STRIPED_SECTION BufferISR() {
  for (int i = 0; i < 2; ++i) {
    if (dma_irqn_get_channel_status(DMA_IRQ_1, g_dma_channels[i])) {
      dma_irqn_acknowledge_channel(DMA_IRQ_1, g_dma_channels[i]);
      GenerateAudio(g_buffers[i]);
    }
  }
}

static void InitPWM() {
  gpio_set_function(PWM_PIN, GPIO_FUNC_PWM);

  g_pwm_slice = pwm_gpio_to_slice_num(PWM_PIN);
  g_pwm_channel = pwm_gpio_to_channel(PWM_PIN);

  pwm_config cfg = pwm_get_default_config();
  pwm_init(g_pwm_slice, &cfg, true);
}

static void InitDMA() {
  g_dma_timer = dma_claim_unused_timer(true);
  dma_timer_set_fraction(g_dma_timer, TIMER_NUMERATOR, TIMER_DENOMINATOR);

  // 2 ping ponging DMA channels.
  for (int i = 0; i < 2; ++i) {
    g_dma_channels[i] = dma_claim_unused_channel(true);
  }
  for (int i = 0; i < 2; ++i) {
    dma_channel_config cfg = dma_channel_get_default_config(g_dma_channels[i]);
    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_16);
    channel_config_set_read_increment(&cfg, true);
    channel_config_set_ring(&cfg, false, BUFFER_SIZE_BITS);
    channel_config_set_write_increment(&cfg, false);
    channel_config_set_dreq(&cfg, dma_get_timer_dreq(g_dma_timer));
    channel_config_set_chain_to(&cfg, g_dma_channels[1-i]);
    dma_channel_configure(g_dma_channels[i], &cfg, &pwm_hw->slice[g_pwm_slice].cc, g_buffers[i], BUFFER_SIZE, false);

    dma_irqn_set_channel_enabled(DMA_IRQ_1, g_dma_channels[i], true);
  }

  irq_add_shared_handler(DMA_IRQ_1, BufferISR, PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
  irq_set_priority(DMA_IRQ_1, 0x40);
  irq_set_enabled(DMA_IRQ_1, true);
  dma_channel_start(g_dma_channels[0]);
}

void InitAudio() {
  ayemu_init(&g_ay);
  ayemu_set_sound_format(&g_ay, SAMPLE_FREQ, 1, 8);

  for (int i = 0; i < 2; ++i) {
    GenerateAudio(g_buffers[i]);
  }

  InitPWM();
  InitDMA();
}
