#include <assert.h>
#include <stdlib.h>
#include "pico/stdlib.h"

#include "scan_out.h"
#include "section.h"
#include "sys80.h"
#include "video_dma.h"

enum {
  OP_MASK       = 0x03,
  OP_NOP        = 0x00,
  OP_READ       = 0x01,
  OP_WRITE      = 0x02,
  OP_STREAM     = 0x03,

  OP_STEP_MASK  = 0x30,
  OP_STEP_0     = 0x00,
  OP_STEP_1     = 0x10,
  OP_STEP_128   = 0x20,
  OP_STEP_1_127 = 0x30,
};

static int g_last_y;

void UpdateVideoMem() {
  if (IsSys80FifoEmpty()) {
    return;
  }

  // Process up to one memory access per scanline and as they arrive during vertical non-display area.
  /*int y = g_logical_y;
  if (y < 240 && y == g_last_y) {
    return;
  }
  g_last_y = y;*/

  // Pop the streamed data. One dummy value remains in the FIFO.
  int streamed = PopSys80Fifo();

  int device = g_sys80_regs.mem_access.device;
  int address = Unswizzle16BitSys80Reg(g_sys80_regs.mem_access.address);
  int op = g_sys80_regs.mem_access.operation;

  switch (op & OP_MASK) {
  case OP_READ:
    g_sys80_regs.mem_access.data = ReadVideoMemByte(device, address);
    break;
  case OP_WRITE:
    WriteVideoMemByte(device, address, g_sys80_regs.mem_access.data);
    break;
  case OP_STREAM:
    WriteVideoMemByte(device, address, streamed);
    break;
  }

  switch (op & OP_STEP_MASK) {
  case OP_STEP_1:
    g_sys80_regs.mem_access.address = Swizzle16BitSys80Reg(address + 1);
    break;
  case OP_STEP_128:
    g_sys80_regs.mem_access.address = Swizzle16BitSys80Reg(address + 128);
    break;
  case OP_STEP_1_127:
    if (address & 1) {
      g_sys80_regs.mem_access.address = Swizzle16BitSys80Reg(address + 127);
    } else {
      g_sys80_regs.mem_access.address = Swizzle16BitSys80Reg(address + 1);
    }
    break;
  }

  // Only pop the dummy value after the memory access is complete and it's safe
  // for the Z80 bus master to change the memory access registers.
  PopSys80Fifo();
}