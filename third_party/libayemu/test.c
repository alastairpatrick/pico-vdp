/* Test AY/YM emulation programm */

#include <stdio.h>
#include <stdlib.h>
#include <memory.h>

#include "ayemu.h"
#include "../../sys80.h"

#define MUTE       0
#define TONE_A	   1
#define TONE_B     2
#define TONE_C     4
#define NOISE_A    8
#define NOISE_B   16
#define NOISE_C   32


static void gen_sound (int tonea, int toneb, int tonec, int noise, int control, int vola, int volb, int volc, int envfreq, int envstyle)
{
  int n, len;
  volatile uint16_t* regs = g_sys80_regs.ay;

  /* setup regs */
  regs[0] = tonea & 0xff;
  regs[1] = tonea >> 8;
  regs[2] = toneb & 0xff;
  regs[3] = toneb >> 8;
  regs[4] = tonec & 0xff;
  regs[5] = tonec >> 8;
  regs[6] = noise;
  regs[7] = (~control) & 0x3f; 	/* invert bits 0-5 */
  regs[10] = vola; 		/* included bit 4 */
  regs[11] = volb;
  regs[12] = volc;
  regs[13] = envfreq & 0xff;
  regs[14] = envfreq >> 8;
  regs[15] = envstyle;
}


struct test_t {
  char *name;
  int tonea, toneb, tonec, noise, control, vola, volb, volc, envfreq, envstyle;
} 
testcases [] = {

  {"Mute: tones 400, volumes 15 noise 15", 
   400, 400, 400, 15 /*Noise*/, MUTE /* Ctrl */, 15, 15, 15, /* env freq,style */4000, 4 },
  {"Mute: tones 400, noise 25, volumes 31 (use env)", 
   400, 400, 400, 25, MUTE, 31, 31, 31, /* env freq,style */4000, 4 },

  {"Channel A: tone 400, volume 0", 
   400, 0, 0, 0, TONE_A, 0, 0, 0, /* env freq,style */0, 0 },
  {"Channel A: tone 400, volume 5",  
   400, 0, 0, 0, TONE_A, 5, 0, 0, 0, 0},
  {"Channel A: tone 400, volume 10", 
   400, 0, 0, 0, TONE_A, 10, 0, 0, 0, 0},
  {"Channel A: tone 400, volume 15", 
   400, 0, 0, 0, TONE_A, 15, 0, 0, 0, 0},

  {"Channel B: tone 400, volume 0", 
   0, 400, 0, 0/*Noise*/, TONE_B /* Ctrl */, 0, 0, 0, /* env freq,style */0, 0},
  {"Channel B: tone 400, volume 5", 
   0, 400, 0, 0/*Noise*/, TONE_B /* Ctrl */, 0, 5, 0, /* env freq,style */0, 0},
  {"Channel B: tone 400, volume 10", 
   0, 400, 0, 0/*Noise*/, TONE_B /* Ctrl */, 0, 10, 0, /* env freq,style */0, 0},
  {"Channel B: tone 400, volume 15", 
   0, 400, 0, 0/*Noise*/, TONE_B /* Ctrl */, 0, 15, 0, /* env freq,style */0, 0},

  {"Channel C: tone 400, volume 0", 
   0, 0, 400, 0/*Noise*/, TONE_C /* Ctrl */, 0, 0, 0, /* env freq,style */0, 0},
  {"Channel C: tone 400, volume 5", 
   0, 0, 400, 0/*Noise*/, TONE_C /* Ctrl */, 0, 0, 5, /* env freq,style */0, 0},
  {"Channel C: tone 400, volume 10", 
   0, 0, 400, 0/*Noise*/, TONE_C /* Ctrl */, 0, 0, 10, /* env freq,style */0, 0},
  {"Channel C: tone 400, volume 15", 
   0, 0, 400, 0/*Noise*/, TONE_C /* Ctrl */, 0, 0, 15, /* env freq,style */0, 0},

  {"Channel B: noise period = 0, volume = 15", 
   0, 3000, 0, 0, NOISE_B, 0, 15, 0, 0, 0},
  {"Channel B: noise period = 5, volume = 15", 
   0, 3000, 0, 5, NOISE_B, 0, 15, 0, 0, 0},
  {"Channel B: noise period = 10, volume = 15", 
   0, 3000, 0, 10, NOISE_B, 0, 15, 0, 0, 0},
  {"Channel B: noise period = 15, volume = 15", 
   0, 3000, 0, 15, NOISE_B, 0, 15, 0, 0, 0},
  {"Channel B: noise period = 20, volume = 15", 
   0, 3000, 0, 20, NOISE_B, 0, 15, 0, 0, 0},
  {"Channel B: noise period = 25, volume = 15", 
   0, 3000, 0, 25, NOISE_B, 0, 15, 0, 0, 0},
  {"Channel B: noise period = 31, volume = 15", 
   0, 3000, 0, 31, NOISE_B, 0, 15, 0, 0, 0},

  {"Channel A: tone 400, volume = 15, envelop 0 freq 4000", 
   400, 0, 0, 0, TONE_A, 15 | 0x10, 0, 0, 4000, 0},
  {"Channel A: tone 400, volume = 15, envelop 1 freq 4000", 
   400, 0, 0, 0, TONE_A, 15 | 0x10, 0, 0, 4000, 1},
  {"Channel A: tone 400, volume = 15, envelop 2 freq 4000", 
   400, 0, 0, 0, TONE_A, 15 | 0x10, 0, 0, 4000, 2},
  {"Channel A: tone 400, volume = 15, envelop 3 freq 4000", 
   400, 0, 0, 0, TONE_A, 15 | 0x10, 0, 0, 4000, 3},
  {"Channel A: tone 400, volume = 15, envelop 4 freq 4000", 
   400, 0, 0, 0, TONE_A, 15 | 0x10, 0, 0, 4000, 4},
  {"Channel A: tone 400, volume = 15, envelop 5 freq 4000", 
   400, 0, 0, 0, TONE_A, 15 | 0x10, 0, 0, 4000, 5},
  {"Channel A: tone 400, volume = 15, envelop 6 freq 4000", 
   400, 0, 0, 0, TONE_A, 15 | 0x10, 0, 0, 4000, 6},
  {"Channel A: tone 400, volume = 15, envelop 7 freq 4000", 
   400, 0, 0, 0, TONE_A, 15 | 0x10, 0, 0, 4000, 7},
  {"Channel A: tone 400, volume = 15, envelop 8 freq 4000", 
   400, 0, 0, 0, TONE_A, 15 | 0x10, 0, 0, 4000, 8},
  {"Channel A: tone 400, volume = 15, envelop 9 freq 4000", 
   400, 0, 0, 0, TONE_A, 15 | 0x10, 0, 0, 4000, 9},
  {"Channel A: tone 400, volume = 15, envelop 10 freq 4000", 
   400, 0, 0, 0, TONE_A, 15 | 0x10, 0, 0, 4000, 10},
  {"Channel A: tone 400, volume = 15, envelop 11 freq 4000", 
   400, 0, 0, 0, TONE_A, 15 | 0x10, 0, 0, 4000, 11},
  {"Channel A: tone 400, volume = 15, envelop 12 freq 4000", 
   400, 0, 0, 0, TONE_A, 15 | 0x10, 0, 0, 4000, 12},
  {"Channel A: tone 400, volume = 15, envelop 13 freq 4000", 
   400, 0, 0, 0, TONE_A, 15 | 0x10, 0, 0, 4000, 13},
  {"Channel A: tone 400, volume = 15, envelop 14 freq 4000", 
   400, 0, 0, 0, TONE_A, 15 | 0x10, 0, 0, 400, 14},
  {"Channel A: tone 400, volume = 15, envelop 15 freq 4000", 
   400, 0, 0, 0, TONE_A, 15 | 0x10, 0, 0, 4000, 15},
};


void TestAudio(int test)
{ 
  test %= count_of(testcases);
  const char* name = testcases[test].name;

  printf ("Test %d: %s\n", test, name);
  gen_sound (testcases[test].tonea, testcases[test].toneb, 
  testcases[test].tonec, testcases[test].noise,
  testcases[test].control, testcases[test].vola,
  testcases[test].volb, testcases[test].volc,
  testcases[test].envfreq, testcases[test].envstyle);
}
