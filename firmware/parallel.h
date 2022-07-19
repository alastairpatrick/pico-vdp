#ifndef PARALLEL_H
#define PARALLEL_H

typedef void (*ParallelMain)();
typedef void (*ParallelEntry)(const void* ctx, int core_num);

typedef struct {
  ParallelEntry entry;
} ParallelContext;

void Parallel(const void* ctx);

void InitParallel(ParallelMain core1_main);

#endif  // PARALLEL_H
