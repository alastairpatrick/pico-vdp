#include "hardware/dma.h"
#include "hardware/pio.h"

#include "pins.h"
#include "section.h"
#include "sys80.h"

#include "sys80.pio.h"

uint8_t DMA_SECTION g_registers[256] __attribute__ ((aligned(256)));                    // Read/written via port #1
static DMA_SECTION uint32_t g_dummy;

static uint g_read_addr_channel;
static uint g_write_addr_channel;
static uint g_await_read_channel;
static uint g_read_register_channel;
static uint g_await_write_channel;
static uint g_write_register_channel;

static dma_channel_config DefaultChannelConfig(uint channel) {
  dma_channel_config dma_cfg = dma_channel_get_default_config(channel);
  channel_config_set_transfer_data_size(&dma_cfg, DMA_SIZE_32);
  channel_config_set_read_increment(&dma_cfg, false);
  channel_config_set_write_increment(&dma_cfg, false);
  channel_config_set_irq_quiet(&dma_cfg, true);
  return dma_cfg;
}

static void InitSM0() {
  dma_channel_config dma_cfg;

  // This channel reads from the RX FIFO of SM0 and writes the WRITE_ADDR register of another DMA channel.
  dma_cfg = DefaultChannelConfig(g_write_addr_channel);
  channel_config_set_dreq(&dma_cfg, DREQ_PIO1_RX0);
  channel_config_set_chain_to(&dma_cfg, g_read_addr_channel);
  dma_channel_configure(g_write_addr_channel, &dma_cfg, &dma_hw->ch[g_write_register_channel].write_addr, &SYS80_PIO->rxf[0], 1, false);

  // This channel reads from the RX FIFO of SM0 and writes the READ_ADDR register of another DMA channel.
  dma_cfg = DefaultChannelConfig(g_read_addr_channel);
  channel_config_set_dreq(&dma_cfg, DREQ_PIO1_RX0);
  channel_config_set_chain_to(&dma_cfg, g_write_addr_channel);
  dma_channel_configure(g_read_addr_channel, &dma_cfg, &dma_hw->ch[g_read_register_channel].read_addr, &SYS80_PIO->rxf[0], 1, true);

  // Load high 24-bits of g_registers into Y register to it can be combined with register index to give the address
  // of the register in internal SRAM.
  SYS80_PIO->txf[0] = ((uint32_t) g_registers) >> 8;
  pio_sm_exec(SYS80_PIO, 0, pio_encode_pull(false, true));
  pio_sm_exec(SYS80_PIO, 0, pio_encode_mov(pio_y, pio_osr));

  sys80_program_init(SYS80_PIO, 0, pio_add_program(SYS80_PIO, &sys80_rsel_program));
}

static void InitSM1() {
  dma_channel_config dma_cfg;

  // This channel reads a register value from internal SRAM and writes if the the TX FIFO of SM1.
  dma_cfg = DefaultChannelConfig(g_read_register_channel);
  channel_config_set_transfer_data_size(&dma_cfg, DMA_SIZE_8);
  channel_config_set_chain_to(&dma_cfg, g_await_read_channel);
  dma_cfg.ctrl |= DMA_CH1_CTRL_TRIG_HIGH_PRIORITY_BITS;
  dma_channel_configure(g_read_register_channel, &dma_cfg, &SYS80_PIO->txf[1], g_registers, 1, false);

  // This channel reads a dummy value from the RX FIFO of SM1 then chains to another channel.
  dma_cfg = DefaultChannelConfig(g_await_read_channel);
  channel_config_set_dreq(&dma_cfg, DREQ_PIO1_RX1);
  channel_config_set_chain_to(&dma_cfg, g_read_register_channel);
  dma_cfg.ctrl |= DMA_CH1_CTRL_TRIG_HIGH_PRIORITY_BITS;
  dma_channel_configure(g_await_read_channel, &dma_cfg, &g_dummy, &SYS80_PIO->rxf[1], 1, true);

  sys80_program_init(SYS80_PIO, 1, pio_add_program(SYS80_PIO, &sys80_read_rdat_program));
}

static void InitSM2() {
  dma_channel_config dma_cfg;

  // This channel reads a register value from the RX FIFO of SM2 and writes it to internal SRAM.
  dma_cfg = DefaultChannelConfig(g_write_register_channel);
  channel_config_set_dreq(&dma_cfg, DREQ_PIO1_RX2);
  channel_config_set_transfer_data_size(&dma_cfg, DMA_SIZE_8);
  channel_config_set_chain_to(&dma_cfg, g_await_write_channel);
  dma_channel_configure(g_write_register_channel, &dma_cfg, g_registers, &SYS80_PIO->rxf[2], 1, false);

  // This channel reads a dummy value from the RX FIFO of SM2 then chains to another channel.
  dma_cfg = DefaultChannelConfig(g_await_write_channel);
  channel_config_set_dreq(&dma_cfg, DREQ_PIO1_RX2);
  channel_config_set_chain_to(&dma_cfg, g_write_register_channel);
  dma_channel_configure(g_await_write_channel, &dma_cfg, &g_dummy, &SYS80_PIO->rxf[2], 1, true);

  sys80_program_init(SYS80_PIO, 2, pio_add_program(SYS80_PIO, &sys80_write_rdat_program));
}

static void InitSM3() {
  sys80_program_init(SYS80_PIO, 3, pio_add_program(SYS80_PIO, &sys80_fifo_program));
}

void InitSys80() {
  g_read_addr_channel = dma_claim_unused_channel(true);
  g_write_addr_channel = dma_claim_unused_channel(true);
  g_await_read_channel = dma_claim_unused_channel(true);
  g_read_register_channel = dma_claim_unused_channel(true);
  g_await_write_channel = dma_claim_unused_channel(true);
  g_write_register_channel = dma_claim_unused_channel(true);

  InitSM0();
  InitSM1();
  InitSM2();
  InitSM3();
}
