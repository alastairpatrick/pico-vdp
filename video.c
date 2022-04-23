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

#include "video.h"

#include "pins.h"
#include "video.pio.h"

#define MAX_HORZ_DISPLAY_RES 1024
#define MAX_VERT_RES 806
#define DISPLAY_LINE_COUNT 2

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

static uint32_t g_display_lines[DISPLAY_LINE_COUNT][MAX_HORZ_DISPLAY_RES / 4];  // double buffered

static DMAControlBlock g_dma_control_blocks[MAX_VERT_RES * 5 + 1];
static int g_dma_data_chan, g_dma_ctrl_chan;

static int g_current_line;
static int g_line_width;
static LineRenderer g_line_renderer;

static DMAControlBlock MakeControlBlock(uint32_t* read_addr, int transfer_count) {
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

static void InitControlBlocks(const VideoTiming* timing, int horizontal_repetitions, int vertical_repetitions) {
  const AxisTiming* horizontal = &timing->horizontal;
  assert(horizontal->display_pixels <= MAX_HORZ_DISPLAY_RES);
  assert(horizontal->display_pixels % (horizontal_repetitions * sizeof(uint32_t)) == 0);
  assert(horizontal->front_porch_pixels % horizontal_repetitions == 0);
  assert(horizontal->sync_pixels % horizontal_repetitions == 0);
  assert(horizontal->back_porch_pixels % horizontal_repetitions == 0);

  const AxisTiming* vertical = &timing->vertical;
  assert(vertical->display_pixels % vertical_repetitions == 0);
  assert(vertical->display_pixels + vertical->front_porch_pixels + vertical->sync_pixels + vertical->back_porch_pixels <= MAX_VERT_RES);

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

  static uint32_t display_cmd, fporch_cmd, fporchi_cmd, fporchv_cmd, hsync_cmd, hsyncv_cmd, bporch_cmd, bporchv_cmd, vporch_cmd, vsync_cmd;

  vporch_cmd = MakeCmd(horizontal->display_pixels / horizontal_repetitions + 1, false, false, false, video_offset_handle_non_display);
  display_cmd = MakeCmd(horizontal->display_pixels / horizontal_repetitions, false, false, false, video_offset_handle_display);
  vsync_cmd = MakeCmd(horizontal->display_pixels / horizontal_repetitions + 1, false, true /*vsync*/, false, video_offset_handle_non_display);

  fporch_cmd = MakeCmd(horizontal->front_porch_pixels / horizontal_repetitions - 4, false, false, false, video_offset_handle_non_display);
  fporchi_cmd = MakeCmd(horizontal->front_porch_pixels / horizontal_repetitions - 4, false, false, true /* raise_irq */, video_offset_handle_non_display);
  fporchv_cmd = MakeCmd(horizontal->front_porch_pixels / horizontal_repetitions - 4, false, true /* vsync */, false, video_offset_handle_non_display);

  hsync_cmd = MakeCmd(horizontal->sync_pixels / horizontal_repetitions - 2, true /* hsync */, false, false, video_offset_handle_non_display);
  hsyncv_cmd = MakeCmd(horizontal->sync_pixels / horizontal_repetitions - 2, true /* hsync */, true /* vsync */, false, video_offset_handle_non_display);

  bporch_cmd = MakeCmd(horizontal->back_porch_pixels / horizontal_repetitions - 3, false, false, false, video_offset_handle_non_display);
  bporchv_cmd = MakeCmd(horizontal->back_porch_pixels / horizontal_repetitions - 3, false, true /* vsync */, false, video_offset_handle_non_display);

  DMAControlBlock* control = g_dma_control_blocks;

  int display_line_idx = 0;
  int display_line_size = horizontal->display_pixels / horizontal_repetitions / sizeof(uint32_t);

  for (int y = 0; y < vertical->back_porch_pixels; ++y) {
    bool raise_irq = ((vertical->back_porch_pixels - y - 1) % vertical_repetitions == 0) && (vertical->back_porch_pixels - y < vertical_repetitions * DISPLAY_LINE_COUNT);

    *(control++) = MakeControlBlock(&vporch_cmd, 1);
    *(control++) = MakeControlBlock(raise_irq ? &fporchi_cmd : &fporch_cmd, 1);
    *(control++) = MakeControlBlock(&hsync_cmd, 1);
    *(control++) = MakeControlBlock(&bporch_cmd, 1);
  }

  for (int y = 0; y < vertical->display_pixels; y += vertical_repetitions) {
    for (int i = 0; i < vertical_repetitions; ++i) {
      bool raise_irq = (i == vertical_repetitions-1) && (y < vertical->display_pixels - DISPLAY_LINE_COUNT);

      *(control++) = MakeControlBlock(&display_cmd, 1);
      *(control++) = MakeControlBlock(g_display_lines[display_line_idx], display_line_size);
      *(control++) = MakeControlBlock(raise_irq ? &fporchi_cmd : &fporch_cmd, 1);
      *(control++) = MakeControlBlock(&hsync_cmd, 1);
      *(control++) = MakeControlBlock(&bporch_cmd, 1);
    }

    ++display_line_idx;
    if (display_line_idx == DISPLAY_LINE_COUNT) {
      display_line_idx = 0;
    }
  }

  for (int y = 0; y < vertical->front_porch_pixels; ++y) {
    *(control++) = MakeControlBlock(&vporch_cmd, 1);
    *(control++) = MakeControlBlock(&fporch_cmd, 1);
    *(control++) = MakeControlBlock(&hsync_cmd, 1);
    *(control++) = MakeControlBlock(&bporch_cmd, 1);
  }

  for (int y = 0; y < vertical->sync_pixels; ++y) {
    *(control++) = MakeControlBlock(&vsync_cmd, 1);
    *(control++) = MakeControlBlock(&fporchv_cmd, 1);
    *(control++) = MakeControlBlock(&hsyncv_cmd, 1);
    *(control++) = MakeControlBlock(&bporchv_cmd, 1);
  }

  *(control++) = MakeControlBlock(NULL, 0);
}

static void __not_in_flash_func(RestartVideo)() {
  g_current_line = 0;
  dma_channel_set_read_addr(g_dma_ctrl_chan, g_dma_control_blocks, true);
}

static void __not_in_flash_func(FrameISR)() {
  if (dma_irqn_get_channel_status(DMA_IRQ_IDX, g_dma_data_chan)) {
    dma_irqn_acknowledge_channel(DMA_IRQ_IDX, g_dma_data_chan);
    RestartVideo();
  }
}

static void __not_in_flash_func(LineISR)() {
  pio_interrupt_clear(PIO, 0);
  g_line_renderer(g_display_lines[g_current_line & 1], g_current_line, g_line_width);
  ++g_current_line;
}

void InitVideo(const VideoTiming* timing, int horizontal_repetitions, int vertical_repetitions, LineRenderer renderer) {
  g_current_line = 0;
  g_line_renderer = renderer;
  g_line_width = timing->horizontal.display_pixels / horizontal_repetitions;

  set_sys_clock_pll(timing->vco_freq, timing->vco_div1, timing->vco_div2);

  InitControlBlocks(timing, horizontal_repetitions, vertical_repetitions);

  uint offset = pio_add_program(PIO, &video_program);
  video_program_init(PIO, 0, offset, VIDEO_PINS, SYNC_PINS, timing->pio_clk_div * horizontal_repetitions);

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

  dma_irqn_set_channel_enabled(DMA_IRQ_IDX, g_dma_data_chan, true);
  irq_add_shared_handler(DMA_IRQ_0 + DMA_IRQ_IDX, FrameISR, PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
  irq_set_enabled(DMA_IRQ_0 + DMA_IRQ_IDX, true);

  pio_set_irq0_source_enabled(PIO, pis_interrupt0, true);
  irq_set_exclusive_handler(PIO_IRQ, LineISR);
  irq_set_enabled(PIO_IRQ, true);

  RestartVideo();
}
