#include <stdlib.h>
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/structs/bus_ctrl.h"

#include "pins.h"
#include "sys80.h"

#include "sys80_test.pio.h"

#define PIO pio0
#define SM 0

static void Init() {
  uint offset = pio_add_program(PIO, &sim_request_program);
  sim_request_program_init(PIO, SM, offset, DATA_PINS, CS_PINS);

  gpio_init(RD_PIN);
  gpio_set_dir(RD_PIN, GPIO_OUT);
  gpio_put(RD_PIN, 1);

  // It's safe to turn off PIO0 pin synchronizers for these tests because it is only the RP2040 itself
  // that sets input states, rather than an external asynchronous device like a Z80. Leaving the pin
  // synchronizer enabled would artificially increase measured latency by two cycles.
  //
  // In contrast, the PIO1 pin synchronizers should be enabled; when used in a real system,
  // its inputs are asynchronous. We want to include pin synchronizer delay in measurements.
  pio0->input_sync_bypass = 0xFFFFFFFF;
}

static int SimulateIORead(int cs) {
  gpio_put(RD_PIN, 0);

  pio_sm_put(PIO, SM, ~(1 << cs));

  int data =  pio_sm_get_blocking(PIO, SM) & 0xFF;

  gpio_put(RD_PIN, 1);

  return data;
}

static void SimulateIOWrite(int cs, int data) {
  pio_sm_set_pindirs_with_mask(pio1, 0, 0xFF << DATA_PINS, 0xFF << DATA_PINS);
  pio_sm_set_pins_with_mask(pio1, 0, data << DATA_PINS, 0xFF << DATA_PINS);

  pio_sm_put(PIO, SM, ~(1 << cs));
  pio_sm_get_blocking(PIO, SM);

  pio_sm_set_pindirs_with_mask(pio1, 0, 0, 0xFF << DATA_PINS);
}

void TestSys80() {
  static int v;
  Init();

  // Test reading register 0 through port #1
  for (int i = 0; i < 256; ++i) {
    g_registers[0] = i;
    int actual = SimulateIORead(1);
    assert(actual == i);
  }

  // Test writing register 0 through port #1.
  for (int i = 0; i < 256; ++i) {
    SimulateIOWrite(1, i);
    assert(g_registers[0] == i);
  }

  // Test reading and writing selected register through port #0
  for (int i = 0; i < 256; ++i) {
    SimulateIOWrite(0, i);
    int actual = SimulateIORead(0);
    assert(actual == i);
  }

  // Test selecting and reading registers.
  for (int i = 0; i < 256; ++i) {
    SimulateIOWrite(0, i);
    g_registers[i] = i;
    int actual = SimulateIORead(1);
    assert(actual == i);
  }

  // Test selecting and writing read/write registers.
  for (int i = 0; i < 128; ++i) {
    SimulateIOWrite(0, i);
    g_registers[i] = 7;
    SimulateIOWrite(1, i);
    assert(g_registers[i] == i);
  }

  // Test selecting and writing read-only registers.
  for (int i = 128; i < 256; ++i) {
    SimulateIOWrite(0, i);
    g_registers[i] = 7;
    g_registers[i-128] = 7;
    SimulateIOWrite(1, i);
    assert(g_registers[i - 128] == i);
    assert(g_registers[i] == 7);
  }

  // Test writing to ring buffer through port #2
  for (int i = 0; i < RING_SIZE; ++i) {
    int written = v = SimulateIORead(2);
    assert(written == ((i*4) & 0xFF));
    
    SimulateIOWrite(2, 0x10);
    SimulateIOWrite(2, 0x20);
    SimulateIOWrite(2, 0x30);
    SimulateIOWrite(2, 0x40);
  }

  for (int i = 0; i < RING_SIZE; ++i) {
    assert(g_ring_buffer[i] == 0x40302010);
  }

  // Ring buffer wraps around
  for (int i = 0; i < RING_SIZE; ++i) {
    SimulateIOWrite(2, 0x50);
    SimulateIOWrite(2, 0x60);
    SimulateIOWrite(2, 0x70);
    SimulateIOWrite(2, 0x80);
  }

  for (int i = 0; i < RING_SIZE; ++i) {
    assert(g_ring_buffer[i] == 0x80706050);
  }
}
