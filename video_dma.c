#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/pll.h"

#include "video_dma.h"

#include "pins.h"
#include "scan_out.h"
#include "section.h"

#include "video.pio.h"

#define MAX_HORZ_DISPLAY_RES 1024
#define MAX_VERT_RES 806
#define DISPLAY_LINE_COUNT 2
#define DISPLAY_GUARD 16

#define DMA_IRQ_IDX 0
#define PIO pio0
#define PIO_IRQ PIO0_IRQ_0

typedef struct {
  int32_t transfer_count;
  uint32_t* read_addr;
} DMAControlBlock;

const VideoTiming g_timing640_480 = {
  1512 * MHZ, 6, 2,  // 1512MHZ / 6 / 2 = 126MHz
  640,               // 126MHZ / (640/256) = 50.4MHZ
                     // 50.4MHZ / 2 cycles/pixel = 25.2MHZ =~ 25.175MHz +- 0.5%
  { 640, 16, 96, 48 },
  { 480, 10, 2, 33 },
};

const VideoTiming g_timing800_600 = {
  1440 * MHZ, 6, 2,  // 1440MHZ / 6 / 2 = 120MHz
  384,               // 120MHZ / (384/256) = 80MHZ
                     // 80MHZ / 2 cycles/pixel = 40MHZ
  { 800, 40, 128, 88 },
  { 600, 1, 4, 23 },
};

const VideoTiming g_timing1024_768 = {
  1560 * MHZ, 6, 2,  // 1560MHZ / 6 / 2 = 130MHz
  256,               // 130MHZ / (256/256) = 130MHZ
                     // 130MHz / 2 cycles/pixel = 65MHZ
  { 1024, 24, 136, 160 },
  { 768, 3, 6, 29 },
};

int g_blank_logical_width;
int g_total_logical_width;

static VideoTiming g_timing;
static int g_horz_shift, g_vert_shift;

// double buffered
static uint8_t DMA_SECTION g_display_lines[DISPLAY_LINE_COUNT][MAX_HORZ_DISPLAY_RES + 2*DISPLAY_GUARD];

static DMAControlBlock g_dma_control_blocks[MAX_VERT_RES * 3];
static int g_dma_data_chan, g_dma_ctrl_chan;

static int g_logical_y;

static DMAControlBlock MakeControlBlock(void* read_addr, int transfer_count) {
  DMAControlBlock result = {
    .read_addr = read_addr,
    .transfer_count = transfer_count
  };
  return result;
}

static uint32_t MakeCmd(int run_pixels, bool hsync, bool vsync, bool raise_irq, int handler) {
  int sync_state = 0b11;
  if (hsync) {
    sync_state &= 0b01;   // Set first sync bit active (low)
  }
  if (vsync) {
    sync_state &= 0b10;   // Set secondsync bit active (low)
  }
  int pio_instruction = raise_irq ? pio_encode_irq_set(false, 0) : pio_encode_nop();
  pio_instruction |= pio_encode_sideset_opt(2, sync_state);
  return (run_pixels-1) | (pio_instruction << 11) | (handler << 27);
}

static void InitControlBlocks() {
  const VideoAxisTiming* horz = &g_timing.horz;
  const VideoAxisTiming* vert = &g_timing.vert;
  assert(vert->display_pixels + vert->front_porch_pixels + vert->sync_pixels + vert->back_porch_pixels <= MAX_VERT_RES);

  // <-------- vporch --------><- fporch --><- hsync --><- bporch -->
  // <-------- vporch --------><- fporch --><- hsync --><- bporch -->
  // ...
  // <-------- vporch --------><- fporch --><- hsync --><- bporch -->
  // <-------- vporch --------><- fporchi -><- hsync --><- bporch -->     IRQ is raised for a few cycles during back porch to render the first few lines prior to display
  // <-------- vporch --------><- fporch --><- hsync --><- bporch -->
  // <-------- vporch --------><- fporchi -><- hsync --><- bporch -->
  // <-------- display -------><- fporch --><- hsync --><- bporch -->
  // <-------- display -------><- fporchi -><- hsync --><- bporch -->
  // <-------- display -------><- fporch --><- hsync --><- bporch -->
  // <-------- display -------><- fporchi -><- hsync --><- bporch -->
  // <-------- display -------><- fporch --><- hsync --><- bporch -->
  // <-------- display -------><- fporchi -><- hsync --><- bporch -->
  // <-------- display -------><- fporch --><- hsync --><- bporch -->
  // <-------- display -------><- fporchi -><- hsync --><- bporch -->
  // ...
  // <-------- display -------><- fporch --><- hsync --><- bporch -->     IRQ is suspended before the end of the display area once the final line is rendered
  // <-------- display -------><- fporch --><- hsync --><- bporch -->
  // <-------- display -------><- fporch --><- hsync --><- bporch -->
  // <-------- display -------><- fporch --><- hsync --><- bporch -->
  // ...
  // <-------- vporch --------><- fporch --><- hsync --><- bporch -->
  // ...
  // <-------- vsync ---------><- fporchv -><- hsyncv -><- bporchv ->
  // ...

  int display_shifted = horz->display_pixels >> g_horz_shift;
  uint32_t vporch_cmd = MakeCmd(display_shifted + 1, false, false, false, video_offset_handle_non_display);
  uint32_t display_cmd = MakeCmd(display_shifted, false, false, false, video_offset_handle_display);
  uint32_t vsync_cmd = MakeCmd(display_shifted + 1, false, true /*vsync*/, false, video_offset_handle_non_display);

  int front_shifted = horz->front_porch_pixels >> g_horz_shift;
  uint32_t fporch_cmd = MakeCmd(front_shifted - 4, false, false, false, video_offset_handle_non_display);
  uint32_t fporch_irq_cmd = MakeCmd(front_shifted - 4, false, false, true /* raise_irq */, video_offset_handle_non_display);
  uint32_t fporch_vsync_cmd = MakeCmd(front_shifted - 4, false, true /* vsync */, false, video_offset_handle_non_display);

  int sync_shifted = horz->sync_pixels >> g_horz_shift;
  uint32_t hsync_cmd = MakeCmd(sync_shifted - 2, true /* hsync */, false, false, video_offset_handle_non_display);
  uint32_t hsync_vsync_cmd = MakeCmd(sync_shifted - 2, true /* hsync */, true /* vsync */, false, video_offset_handle_non_display);

  int back_shifted = horz->back_porch_pixels >> g_horz_shift;
  uint32_t bporch_cmd = MakeCmd(back_shifted - 3, false, false, false, video_offset_handle_non_display);
  uint32_t bporch_vsync_cmd = MakeCmd(back_shifted - 3, false, true /* vsync */, false, video_offset_handle_non_display);

  static uint32_t display_cmds[1];
  display_cmds[0] = display_cmd;

  static uint32_t vporch_cmds[4];
  vporch_cmds[0] = vporch_cmd;
  vporch_cmds[1] = fporch_cmd;
  vporch_cmds[2] = hsync_cmd;
  vporch_cmds[3] = bporch_cmd;

  static uint32_t vporch_irq_cmds[4];
  vporch_irq_cmds[0] = vporch_cmd;
  vporch_irq_cmds[1] = fporch_irq_cmd;
  vporch_irq_cmds[2] = hsync_cmd;
  vporch_irq_cmds[3] = bporch_cmd;

  static uint32_t vporch_vsync_cmds[4];
  vporch_vsync_cmds[0] = vsync_cmd;
  vporch_vsync_cmds[1] = fporch_vsync_cmd;
  vporch_vsync_cmds[2] = hsync_vsync_cmd;
  vporch_vsync_cmds[3] = bporch_vsync_cmd;

  static uint32_t hporch_cmds[3];
  hporch_cmds[0] = fporch_cmd;
  hporch_cmds[1] = hsync_cmd;
  hporch_cmds[2] = bporch_cmd;

  static uint32_t hporch_irq_cmds[3];
  hporch_irq_cmds[0] = fporch_irq_cmd;
  hporch_irq_cmds[1] = hsync_cmd;
  hporch_irq_cmds[2] = bporch_cmd;

  DMAControlBlock* control = g_dma_control_blocks;

  int irq_count = 0;
  int irq_total = vert->display_pixels >> g_vert_shift;

  int vert_reps = 1 << g_vert_shift;
  for (int y = 0; y < vert->back_porch_pixels; ++y) {
    if (y == (vert->back_porch_pixels - 1) || y == (vert->back_porch_pixels - vert_reps - 1)) {
      *control++ = MakeControlBlock(vporch_irq_cmds, count_of(vporch_cmds));
      ++irq_count;
    } else {
      *control++ = MakeControlBlock(vporch_cmds, count_of(vporch_cmds));
    }
  }

  int display_line_idx = 0;
  int display_line_size = display_shifted / sizeof(uint32_t);

  for (int y = 0; y < vert->display_pixels; y += vert_reps) {
    for (int i = 0; i < vert_reps; ++i) {
      *control++ = MakeControlBlock(display_cmds, count_of(display_cmds));
      *control++ = MakeControlBlock(g_display_lines[display_line_idx] + DISPLAY_GUARD, display_line_size);

      if (i == (vert_reps-1) && (irq_count < irq_total)) {
        *control++ = MakeControlBlock(hporch_irq_cmds, count_of(hporch_cmds));
        ++irq_count;
      } else {
        *control++ = MakeControlBlock(hporch_cmds, count_of(hporch_cmds));
      }
    }

    display_line_idx = 1 - display_line_idx;
  }

  for (int y = 0; y < vert->front_porch_pixels; ++y) {
    *control++ = MakeControlBlock(vporch_cmds, count_of(vporch_cmds));
  }

  for (int y = 0; y < vert->sync_pixels; ++y) {
    *control++ = MakeControlBlock(vporch_vsync_cmds, count_of(vporch_vsync_cmds));
  }

  *control++ = MakeControlBlock(NULL, 0);
}

void STRIPED_SECTION StartVideo() {
  g_logical_y = 0;
  dma_channel_set_read_addr(g_dma_ctrl_chan, g_dma_control_blocks, true);
}

static void STRIPED_SECTION FrameISR() {
  if (dma_irqn_get_channel_status(DMA_IRQ_IDX, g_dma_data_chan)) {
    dma_irqn_acknowledge_channel(DMA_IRQ_IDX, g_dma_data_chan);
    StartVideo();
  }
}

static void STRIPED_SECTION LineISR() {
  if (g_logical_y == 0) {
    pwm_set_counter(VIDEO_PWM, 0);
  }

  pio_interrupt_clear(PIO, 0);

  if (g_logical_y == 0) {
    ScanOutBeginDisplay();
  }

  int logical_width = g_timing.horz.display_pixels >> g_horz_shift;
  ScanOutLine(g_display_lines[g_logical_y & 1] + DISPLAY_GUARD, g_logical_y, logical_width);

  if (g_logical_y == (g_timing.vert.display_pixels >> g_vert_shift) - 1) {
    ScanOutEndDisplay();
  }

  ++g_logical_y;
}

void InitVideo(const VideoTiming* timing) {
  g_timing = *timing;
  set_sys_clock_pll(timing->vco_freq, timing->vco_div1, timing->vco_div2);

  uint offset = pio_add_program(PIO, &video_program);
  video_program_init(PIO, 0, offset, VIDEO_PINS, SYNC_PINS);

  g_dma_data_chan = dma_claim_unused_channel(true);
  g_dma_ctrl_chan = dma_claim_unused_channel(true);

  dma_channel_config dma_cfg = dma_channel_get_default_config(g_dma_data_chan);
  channel_config_set_dreq(&dma_cfg, pio_get_dreq(PIO, 0, true /*is_tx*/));
  channel_config_set_transfer_data_size(&dma_cfg, DMA_SIZE_32);
  channel_config_set_read_increment(&dma_cfg, true);
  channel_config_set_write_increment(&dma_cfg, false);
  channel_config_set_chain_to(&dma_cfg, g_dma_ctrl_chan);
  channel_config_set_irq_quiet(&dma_cfg, true);

  // read address and transfer count set by control channel.
  dma_channel_configure(g_dma_data_chan, &dma_cfg, &PIO->txf[0], NULL, 0, false);

  dma_cfg = dma_channel_get_default_config(g_dma_ctrl_chan);
  channel_config_set_transfer_data_size(&dma_cfg, DMA_SIZE_32);
  channel_config_set_read_increment(&dma_cfg, true);
  channel_config_set_write_increment(&dma_cfg, true);
  channel_config_set_ring(&dma_cfg, true, 3);
  dma_channel_configure(g_dma_ctrl_chan, &dma_cfg, &dma_hw->ch[g_dma_data_chan].al3_transfer_count, NULL, 2, false);

  pwm_config pwm_cfg = pwm_get_default_config();
  pwm_init(VIDEO_PWM, &pwm_cfg, false);
}

void SetVideoResolution(int horz_shift, int vert_shift) {
  assert((g_timing.horz.display_pixels >> horz_shift) <= MAX_HORZ_DISPLAY_RES);

  g_horz_shift = horz_shift;
  g_vert_shift = vert_shift;
  
  InitControlBlocks();

  int pio_clk_div = g_timing.pio_clk_div << horz_shift;
  pio_sm_set_clkdiv_int_frac(PIO, 0, pio_clk_div >> 8, pio_clk_div & 0xFF);

  pwm_set_counter(VIDEO_PWM, 0);

  // Want a period of total_logical_width so set WRAP to total_logical_width-1.
  g_blank_logical_width = (g_timing.horz.front_porch_pixels + g_timing.horz.sync_pixels + g_timing.horz.back_porch_pixels) >> horz_shift;
  g_total_logical_width = (g_timing.horz.display_pixels >> horz_shift) + g_blank_logical_width;
  pwm_set_wrap(VIDEO_PWM, g_total_logical_width - 1);

  // +1 because PIO timing is 2 cycles/pixel.
  int pwm_clk_div = pio_clk_div << (vert_shift + 1);
  assert(pwm_clk_div < 65536);  // 8.4 divider
  pwm_set_clkdiv_int_frac(VIDEO_PWM, pwm_clk_div >> 8, (pwm_clk_div >> 4) & 0xF);

  pwm_set_enabled(VIDEO_PWM, true);
}

void InitVideoInterrupts() {
  dma_irqn_set_channel_enabled(DMA_IRQ_IDX, g_dma_data_chan, true);
  irq_add_shared_handler(DMA_IRQ_0 + DMA_IRQ_IDX, FrameISR, PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
  irq_set_enabled(DMA_IRQ_0 + DMA_IRQ_IDX, true);

  pio_set_irq0_source_enabled(PIO, pis_interrupt0, true);
  irq_set_exclusive_handler(PIO_IRQ, LineISR);
  irq_set_enabled(PIO_IRQ, true);
}
