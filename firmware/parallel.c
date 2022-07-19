#include "hardware/irq.h"
#include "pico/multicore.h"

#include "parallel.h"

#include "section.h"

static ParallelMain g_core1_main;

void STRIPED_SECTION Parallel(const void* cc) {
  sio_hw->fifo_wr = (uint32_t) cc;

  const ParallelContext* ctx = cc;
  ParallelEntry entry = ctx->entry;
  entry(ctx, 0);

  while (!(sio_hw->fifo_st & SIO_FIFO_ST_VLD_BITS)) {
  }

  sio_hw->fifo_rd;
}

static void STRIPED_SECTION FifoISR() {
  while (sio_hw->fifo_st & SIO_FIFO_ST_VLD_BITS) {
    const ParallelContext* ctx = (const ParallelContext*) sio_hw->fifo_rd;
    ParallelEntry entry = ctx->entry;
    entry(ctx, 1);
    sio_hw->fifo_wr = 0;
  }
}

static void STRIPED_SECTION Main() {
  multicore_fifo_clear_irq();
  irq_set_exclusive_handler(SIO_IRQ_PROC1, FifoISR);
  irq_set_enabled(SIO_IRQ_PROC1, true);

  for (;;) {
    g_core1_main();
  }
}

void InitParallel(ParallelMain core1_main) {
  g_core1_main = core1_main;
  multicore_launch_core1(Main);
}
