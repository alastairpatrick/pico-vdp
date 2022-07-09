#include "pico/multicore.h"

#include "parallel.h"

#include "section.h"

void STRIPED_SECTION Parallel(const void* cc) {
  sio_hw->fifo_wr = (uint32_t) cc;

  const ParallelContext* ctx = cc;
  ParallelEntry entry = ctx->entry;
  entry(ctx, 0);

  while (!(sio_hw->fifo_st & SIO_FIFO_ST_VLD_BITS)) {
  }

  sio_hw->fifo_rd;
}

static void STRIPED_SECTION Main() {
  for (;;) {
    while (!(sio_hw->fifo_st & SIO_FIFO_ST_VLD_BITS)) {
    }

    const ParallelContext* ctx = (const ParallelContext*) sio_hw->fifo_rd;
    ParallelEntry entry = ctx->entry;
    entry(ctx, 1);
    sio_hw->fifo_wr = 0;
  }
}

void InitParallel() {
  multicore_launch_core1(Main);
}
