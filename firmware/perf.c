#include "hardware/structs/xip_ctrl.h"

#include "perf.h"

void InitPerf() {
  systick_hw->csr = M0PLUS_SYST_CSR_CLKSOURCE_BITS | M0PLUS_SYST_CSR_ENABLE_BITS;
  systick_hw->rvr = 0x00FFFFFF;
}

void STRIPED_SECTION EnableXIPCache() {
  hw_set_bits(&xip_ctrl_hw->ctrl, XIP_CTRL_EN_BITS);
}

void STRIPED_SECTION DisableXIPCache() {
  hw_clear_bits(&xip_ctrl_hw->ctrl, XIP_CTRL_EN_BITS);
}
