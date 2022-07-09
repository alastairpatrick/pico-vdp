#ifndef PARALLEL_H
#define PARALLEL_H

typedef void (*ParallelEntry)(const void* ctx, int core_num);

typedef struct {
  ParallelEntry entry;
} ParallelContext;

void Parallel(const void* ctx);

void InitParallel();

#endif  // PARALLEL_H
