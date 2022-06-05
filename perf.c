#include "perf.h"

void InitPerf() {
  systick_hw->csr = M0PLUS_SYST_CSR_CLKSOURCE_BITS | M0PLUS_SYST_CSR_ENABLE_BITS;
  systick_hw->rvr = 0x00FFFFFF;
}

