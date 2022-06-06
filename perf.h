#ifndef PERF_H
#define PERF_H

#include "hardware/structs/systick.h"

#include "section.h"

typedef struct {
  bool enabled;
  int begin_time;
  uint64_t total_time;
  int samples;
  int average_time;
} PerfCounter;

void InitPerf();

static inline int STRIPED_SECTION GetPerfTime() {
  return systick_hw->cvr;
}

static inline void STRIPED_SECTION BeginPerf(PerfCounter* counter) {
  counter->begin_time = systick_hw->cvr;
  counter->enabled = true;
}

static inline void STRIPED_SECTION EndPerf(PerfCounter* counter) {
  int end_time = systick_hw->cvr;

  if (!counter->enabled) {
    return;
  }
  
  counter->total_time += (counter->begin_time - end_time) & 0x007FFFFF;
  ++counter->samples;
  counter->average_time = counter->total_time / counter->samples;
  counter->enabled = false;
}

void EnableXIPCache();
void DisableXIPCache();

#endif  // PERF_H
