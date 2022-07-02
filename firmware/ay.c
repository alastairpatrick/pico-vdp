#include <string.h>
#include "hardware/structs/rosc.h"

#include "ay.h"

#include "sys80.h"

#pragma GCC optimize("O3")

/*int16_t g_vol_table[32] = {
  0, 0, 63, 95, 125, 157, 187, 221,
  289, 377, 505, 601, 751, 949, 1117, 1287,
  1615, 2019, 2430, 2853, 3491, 4292, 5099, 5929,
  7167, 8724, 10289, 11892, 14221, 16995, 19614, 21845,
};*/

int16_t g_vol_table[32] = {
  0, 0, 82, 150, 223, 275, 336, 413,
  517, 639, 771, 875, 1043, 1259, 1469, 1677,
  1989, 2387, 2805, 3207, 3807, 4563, 5319, 6093,
  7253, 8716, 10174, 11626, 13811, 16468, 19164, 21845,
};

static int g_envelopes[16][128];

static bool STRIPED_SECTION AdvanceTime(const AYState* state, int64_t* reset_time, int64_t period) {
  if (*reset_time + period <= state->time) {
    *reset_time += period;
    if (*reset_time + period <= state->time) {
      *reset_time = state->time;
    }
    return true;
  }
  return false;
}

int STRIPED_SECTION GenerateAY(AYState* state, volatile TrackedSys80Reg* regs) {
  // All periods are a multiple of 16 cycles so only need to recalculate every 16 cycles.
  state->time += 16;

  // Noise generator
  int noise_period = ((regs[6].value & 0x1F) << 4);
  if (AdvanceTime(state, &state->noise_reset_time, noise_period)) {
    state->noise_state = rosc_hw->randombit;
  }

  // Envelope generator
  int env_period = ((regs[13].value << 8) + (regs[14].value << 16));
  if (AdvanceTime(state, &state->env_reset_time, env_period)) {
    if (++state->env_pos >= 128) {
      state->env_pos = 64;
    }
  }

  // Envelope generator reset on write to R15.
  if (regs[15].track == 0) {
    regs[15].track = 1;
    state->env_pos = 0;
    state->env_reset_time = state->time;
  }

  // Tone enables in bits 0-2
  // Noise enables in bits 3-5
  int enables = regs[7].value;

  int env_style = regs[15].value & 0xF;
  int env_amplitude = g_envelopes[env_style][state->env_pos];

  int mix = 0;
  for (int j = 0; j < count_of(state->tones); ++j) {
    // Tone generator
    AYTone* tone = &state->tones[j];
    int period = ((regs[j*2].value << 4) + ((regs[j*2 + 1].value & 0x0F) << 12));
    if (AdvanceTime(state, &tone->reset_time, period)) {
      tone->state = !tone->state;
    }

    int amplitude = regs[j + 10].value & 0x1F;
    if (amplitude & 0x10) {
      amplitude = env_amplitude;
    } else {
      amplitude = amplitude * 2 + 1;
    }

    bool tone_disable = (enables >> j) & 0x1;
    bool noise_disable = (enables >> (j+3)) & 0x1;
    if ((tone->state | tone_disable) & (state->noise_state | noise_disable)) {
      mix += g_vol_table[amplitude];
    }
  }

  return mix;
}


void InitAY(AYState* state) {
  memset(state, 0, sizeof(*state));

  for (int i = 4; i < 32; ++i) {
    g_vol_table[i] = (i * 21845) / 31;
  }

  for (int env = 0; env < 16; env++) {
    int hold = 0;
    int dir = (env & 4) ?  1 : -1;
    int vol = (env & 4) ? -1 : 32;

    for (int pos = 0; pos < 128; pos++) {
      if (!hold) {
        vol += dir;

        if (vol < 0 || vol >= 32) {
          if (env & 8) {

            if (env & 2) dir = -dir;
	          vol = (dir > 0) ? 0 : 31;

	          if (env & 1) {
              hold = 1;
              vol = (dir > 0) ? 31 : 0;
            }
          } else {
            vol = 0;
            hold = 1;
          }
	      }
      }

      g_envelopes[env][pos] = vol;
    }
  }
}
