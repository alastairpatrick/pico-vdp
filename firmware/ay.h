#ifndef AY_H
#define AY_H

#include "sys80.h"

typedef struct {
  int64_t reset_time;
  bool state;
} AYTone;

typedef struct {
  int64_t time;
  int64_t noise_reset_time;
  bool noise_state;
  AYTone tones[3];
  int64_t env_reset_time;
  int env_pos;
} AYState;

void InitAY(AYState* state);
int GenerateAY(AYState* state, volatile TrackedSys80Reg* regs);

#endif  // AY_H
