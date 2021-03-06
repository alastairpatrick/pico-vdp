#include "hardware/dma.h"
#include "hardware/pio.h"

#include "pins.h"
#include "section.h"
#include "sys80.h"

#include "sys80.pio.h"

volatile Sys80Registers DMA_SECTION g_sys80_regs __attribute__ ((aligned(512)));                    // Read/written via port #1
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

static void InitRSELProgram() {
  dma_channel_config dma_cfg;

  // This channel reads from the RX FIFO of SM0 and writes the WRITE_ADDR register of another DMA channel.
  dma_cfg = DefaultChannelConfig(g_write_addr_channel);
  channel_config_set_dreq(&dma_cfg, DREQ_PIO1_RX0);
  channel_config_set_chain_to(&dma_cfg, g_read_addr_channel);
  dma_channel_configure(g_write_addr_channel, &dma_cfg, &dma_hw->ch[g_write_register_channel].write_addr, &pio1->rxf[0], 1, false);

  // This channel reads from the RX FIFO of SM0 and writes the READ_ADDR register of another DMA channel.
  dma_cfg = DefaultChannelConfig(g_read_addr_channel);
  channel_config_set_dreq(&dma_cfg, DREQ_PIO1_RX0);
  channel_config_set_chain_to(&dma_cfg, g_write_addr_channel);
  dma_channel_configure(g_read_addr_channel, &dma_cfg, &dma_hw->ch[g_read_register_channel].read_addr, &pio1->rxf[0], 1, true);

  // Load high 23-bits of g_registers into Y register to it can be combined with register index to give the address
  // of the register in internal SRAM.
  pio1->txf[0] = ((uint32_t) &g_sys80_regs) >> 9;
  pio_sm_exec(pio1, 0, pio_encode_pull(false, true));
  pio_sm_exec(pio1, 0, pio_encode_mov(pio_y, pio_osr));

  sys80_program_init(pio1, 0, pio_add_program(pio1, &sys80_rsel_program));
}

static void InitReadRDATProgram() {
  dma_channel_config dma_cfg;

  // This channel reads a register value from internal SRAM and writes if the the TX FIFO of SM1.
  dma_cfg = DefaultChannelConfig(g_read_register_channel);
  channel_config_set_transfer_data_size(&dma_cfg, DMA_SIZE_8);
  channel_config_set_chain_to(&dma_cfg, g_await_read_channel);
  dma_cfg.ctrl |= DMA_CH1_CTRL_TRIG_HIGH_PRIORITY_BITS;
  dma_channel_configure(g_read_register_channel, &dma_cfg, &pio1->txf[1], &g_sys80_regs, 1, false);

  // This channel reads a dummy value from the RX FIFO of SM1 then chains to another channel.
  dma_cfg = DefaultChannelConfig(g_await_read_channel);
  channel_config_set_dreq(&dma_cfg, DREQ_PIO1_RX1);
  channel_config_set_chain_to(&dma_cfg, g_read_register_channel);
  dma_cfg.ctrl |= DMA_CH1_CTRL_TRIG_HIGH_PRIORITY_BITS;
  dma_channel_configure(g_await_read_channel, &dma_cfg, &g_dummy, &pio1->rxf[1], 1, true);

  sys80_program_init(pio1, 1, pio_add_program(pio1, &sys80_read_rdat_program));
}

static void InitWriteRDATProgram() {
  dma_channel_config dma_cfg;

  // This channel reads a register value from the RX FIFO of SM2 and writes it to internal SRAM.
  dma_cfg = DefaultChannelConfig(g_write_register_channel);
  channel_config_set_dreq(&dma_cfg, DREQ_PIO0_RX2);
  channel_config_set_transfer_data_size(&dma_cfg, DMA_SIZE_16);
  channel_config_set_chain_to(&dma_cfg, g_await_write_channel);
  dma_channel_configure(g_write_register_channel, &dma_cfg, &g_sys80_regs, &pio0->rxf[2], 1, false);

  // This channel reads a dummy value from the RX FIFO of SM2 then chains to another channel.
  dma_cfg = DefaultChannelConfig(g_await_write_channel);
  channel_config_set_dreq(&dma_cfg, DREQ_PIO0_RX2);
  channel_config_set_chain_to(&dma_cfg, g_write_register_channel);
  dma_channel_configure(g_await_write_channel, &dma_cfg, &g_dummy, &pio0->rxf[2], 1, true);

  sys80_program_init(pio0, 2, pio_add_program(pio0, &sys80_write_rdat_program));
}

static void InitFifoProgram() {
  sys80_program_init(pio1, 3, pio_add_program(pio1, &sys80_fifo_program));
}

void InitSys80() {
  g_read_addr_channel = dma_claim_unused_channel(true);
  g_write_addr_channel = dma_claim_unused_channel(true);
  g_await_read_channel = dma_claim_unused_channel(true);
  g_read_register_channel = dma_claim_unused_channel(true);
  g_await_write_channel = dma_claim_unused_channel(true);
  g_write_register_channel = dma_claim_unused_channel(true);

  gpio_set_input_enabled(CS0_PIN, true);
  gpio_set_input_enabled(CS1_PIN, true);
  gpio_set_input_enabled(CS2_PIN, true);
  gpio_set_input_enabled(RD_PIN, true);

  for (int i = 0; i < 8; ++i) {
    pio_gpio_init(pio1, DATA_PINS + i);
  }

  InitRSELProgram();
  InitReadRDATProgram();
  InitWriteRDATProgram();
  InitFifoProgram();
}
