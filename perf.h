#ifndef PERF_H
#define PERF_H

#include "hardware/structs/systick.h"

typedef struct {
  bool enabled;
  int begin_time;
  uint64_t total_time;
  int samples;
  int average_time;
} PerfCounter;

void InitPerf();

static inline int GetPerfTime() {
  return systick_hw->cvr;
}

static inline void BeginPerf(PerfCounter* counter) {
  counter->begin_time = GetPerfTime();
  counter->enabled = true;
}

static inline void EndPerf(PerfCounter* counter) {
  int end_time = GetPerfTime();

  if (!counter->enabled) {
    return;
  }
  
  counter->total_time += (counter->begin_time - end_time) & 0x007FFFFF;
  ++counter->samples;
  counter->average_time = counter->total_time / counter->samples;
  counter->enabled = false;
}

#endif  // PERF_H
