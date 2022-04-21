#ifndef SYS80_H
#define SYS80_H

#define RING_BITS 9
#define RING_BYTES (1 << RING_BITS)
#define RING_SIZE (RING_BYTES / 4)

extern uint8_t g_registers[256];
extern uint32_t g_ring_buffer[RING_SIZE];

void InitSys80();

#endif  // SYS80_H
