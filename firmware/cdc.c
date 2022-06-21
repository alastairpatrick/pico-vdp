#include "tusb.h"

#if CFG_TUH_CDC
CFG_TUSB_MEM_SECTION static char serial_in_buffer[64] = { 0 };

void tuh_mount_cb(uint8_t dev_addr)
{
  tuh_cdc_receive(dev_addr, serial_in_buffer, sizeof(serial_in_buffer), true); // schedule first transfer
}

void tuh_umount_cb(uint8_t dev_addr)
{
}

// invoked ISR context
void tuh_cdc_xfer_isr(uint8_t dev_addr, xfer_result_t event, cdc_pipeid_t pipe_id, uint32_t xferred_bytes)
{
  (void) event;
  (void) pipe_id;
  (void) xferred_bytes;

  tu_memclr(serial_in_buffer, sizeof(serial_in_buffer));
  tuh_cdc_receive(dev_addr, serial_in_buffer, sizeof(serial_in_buffer), true); // waiting for next data
}

#endif