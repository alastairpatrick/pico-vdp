#include "hardware/structs/rosc.h"

#include "sys80.h"

#define NUM_TONES 3

typedef struct {
  int64_t reset_time;
  bool state;
} Tone;

static int64_t g_time;
static int64_t g_noise_reset_time;
static bool g_noise_state;
static Tone g_tones[NUM_TONES];
static int64_t g_env_reset_time;
static int g_env_pos;

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

static bool STRIPED_SECTION AdvanceTime(int64_t* reset_time, int64_t period) {
  if (*reset_time + period <= g_time) {
    *reset_time += period;
    if (*reset_time + period <= g_time) {
      *reset_time = g_time;
    }
    return true;
  }
  return false;
}

void STRIPED_SECTION GenerateAY(uint16_t* buffer, int num_samples) {
  for (int i = 0; i < num_samples; ++i) {
    // All periods are a multiple of 16 cycles so only need to recalculate every 16 cycles.
    g_time += 16;

    // Noise generator
    int noise_period = ((g_sys80_regs.ay[6].value & 0x1F) << 4);
    if (AdvanceTime(&g_noise_reset_time, noise_period)) {
      g_noise_state = rosc_hw->randombit;
    }

    // Envelope generator
    int env_period = ((g_sys80_regs.ay[13].value << 8) + (g_sys80_regs.ay[14].value << 16));
    if (AdvanceTime(&g_env_reset_time, env_period)) {
      if (++g_env_pos >= 128) {
        g_env_pos = 64;
      }
    }

    // Envelope generator reset on write to R15.
    if (g_sys80_regs.ay[15].track == 0) {
      g_sys80_regs.ay[15].track = 1;
      g_env_pos = 0;
      g_env_reset_time = g_time;
    }

    // Tone enables in bits 0-2
    // Noise enables in bits 3-5
    int enables = g_sys80_regs.ay[7].value;

    int mix = 0;
    for (int j = 0; j < NUM_TONES; ++j) {
      // Tone generator
      Tone* tone = &g_tones[j];
      int period = ((g_sys80_regs.ay[j*2].value << 4) + ((g_sys80_regs.ay[j*2 + 1].value & 0x0F) << 12));
      if (AdvanceTime(&tone->reset_time, period)) {
        tone->state = !tone->state;
      }

      int amplitude = g_sys80_regs.ay[j + 10].value & 0x1F;
      if (amplitude & 0x10) {
        int env_style = g_sys80_regs.ay[15].value & 0xF;
        amplitude = g_envelopes[env_style][g_env_pos];
      } else {
        amplitude = amplitude * 2 + 1;
      }

      bool tone_disable = (enables >> j) & 0x1;
      bool noise_disable = (enables >> (j+3)) & 0x1;
      if ((tone->state || tone_disable) && (g_noise_state || noise_disable)) {
        mix += g_vol_table[amplitude];
      }
    }

    buffer[i] = mix >> 9;
  }
}


void InitAY() {
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
