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
#define CYCLES_PER_SAMPLE 40
#define SAMPLE_FREQ (AY_FREQ / CYCLES_PER_SAMPLE)


#define BUFFER_SIZE_BITS 8
#define BUFFER_SIZE (1 << BUFFER_SIZE_BITS)

static int g_pwm_slice, g_pwm_channel;
static int g_dma_channels[2];
static int g_dma_timer;

static uint16_t g_buffers[2][BUFFER_SIZE] __attribute__ ((aligned(BUFFER_SIZE)));

void STRIPED_SECTION BufferISR() {
  for (int i = 0; i < 2; ++i) {
    if (dma_irqn_get_channel_status(DMA_IRQ_1, g_dma_channels[i])) {
      dma_irqn_acknowledge_channel(DMA_IRQ_1, g_dma_channels[i]);
      GenerateAY(g_buffers[i], BUFFER_SIZE, CYCLES_PER_SAMPLE);
    }
  }
}

static void InitPWM() {
  gpio_set_function(PWM_PIN, GPIO_FUNC_PWM);

  g_pwm_slice = pwm_gpio_to_slice_num(PWM_PIN);
  g_pwm_channel = pwm_gpio_to_channel(PWM_PIN);

  pwm_config cfg = pwm_get_default_config();
  pwm_config_set_wrap(&cfg, 0xFF);
  pwm_init(g_pwm_slice, &cfg, true);
}

static void InitDMA() {
  g_dma_timer = dma_claim_unused_timer(true);
  int timer_denominator = clock_get_hz(clk_sys) / SAMPLE_FREQ;
  dma_timer_set_fraction(g_dma_timer, 1, timer_denominator);

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
  InitAY();
  InitPWM();
  InitDMA();
}
