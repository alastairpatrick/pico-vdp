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

int16_t g_vol_table[32] = {
  0, 0, 63, 95, 125, 157, 187, 221,
  289, 377, 505, 601, 751, 949, 1117, 1287,
  1615, 2019, 2430, 2853, 3491, 4292, 5099, 5929,
  7167, 8724, 10289, 11892, 14221, 16995, 19614, 21845,
};

static int g_envelopes[16][128];

static bool STRIPED_SECTION AdvanceTime(int64_t* reset_time, int period) {
  if (*reset_time + period <= g_time) {
    *reset_time += period;
    if (*reset_time + period <= g_time) {
      *reset_time = g_time;
    }
    return true;
  }
  return false;
}

void STRIPED_SECTION GenerateAY(uint16_t* buffer, int num_samples, int cycles_per_sample) {
  for (int i = 0; i < num_samples; ++i) {
    int last_time = g_time;
    g_time = last_time + cycles_per_sample;

    // Noise generator
    int noise_period = ((g_sys80_regs.ay[6] & 0x1F) << 16) + 65536;
    if (AdvanceTime(&g_noise_reset_time, noise_period)) {
      g_noise_state = rosc_hw->randombit;
    }

    // Envelope generator
    int env_style = g_sys80_regs.ay[15];
    if ((env_style & 0x80) == 0) {
      g_env_pos = 0;
      g_sys80_regs.ay[15] = env_style | 0x80;
    }
    env_style &= 0x0F;

    int env_period = ((g_sys80_regs.ay[13] << 8) + (g_sys80_regs.ay[14] << 16)) + 256;
    if (AdvanceTime(&g_env_reset_time, env_period)) {
      if (++g_env_pos >= 128) {
        g_env_pos = 64;
      }
    }

    // Tone enables in bits 0-2
    // Noise enables in bits 3-5
    int enables = g_sys80_regs.ay[7];

    int mix = 0;
    for (int j = 0; j < NUM_TONES; ++j) {
      // Tone generator
      Tone* tone = &g_tones[j];
      int period = ((g_sys80_regs.ay[j * 2] << 4) + ((g_sys80_regs.ay[j*2 + 1] & 0x0F) << 12)) + 16;
      if (AdvanceTime(&tone->reset_time, period)) {
        tone->state = !tone->state;
      }

      int amplitude = g_sys80_regs.ay[j + 10] & 0x1F;
      if (amplitude & 0x10) {
        amplitude = g_envelopes[env_style][g_env_pos];
      } else {
        amplitude = amplitude * 2 + 1;
      }

      bool tone_disable = enables & 0x1;
      bool noise_disable = (enables >> 3) & 0x1;
      if ((tone->state || tone_disable) && (g_noise_state || noise_disable)) {
        mix += g_vol_table[amplitude];
      }

      enables >>= 1;
    }

    buffer[i] = mix >> 8;
  }
}


void InitAY() {
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
