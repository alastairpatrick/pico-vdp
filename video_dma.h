#ifndef VIDEO_DMA_H
#define VIDEO_DMA_H

#include "hardware/pwm.h"

#include "section.h"

#define VIDEO_PWM 0

typedef struct {
  int display_pixels;
  int front_porch_pixels;
  int sync_pixels;
  int back_porch_pixels;
} VideoAxisTiming;

typedef struct {
  // See RP2040 datasheet for constraints on PLL parameters.
  int vco_freq;     // Hz
  int vco_div1;
  int vco_div2;

  int pio_clk_div;  // in 1/256 cycles

  VideoAxisTiming horz;
  VideoAxisTiming vert;
} VideoTiming;

typedef void (*VideoRenderer)(uint32_t* dest, int y, int width);

extern const VideoTiming g_timing640_480;
extern const VideoTiming g_timing800_600;
extern const VideoTiming g_timing1024_768;

extern int g_blank_logical_width;
extern int g_total_logical_width;

void InitVideo(const VideoTiming* timing);
void SetVideoResolution(int horz_shift, int vert_shift);
void InitVideoInterrupts();
void StartVideo();

// Counts logical pixels. Zero is beginning of front porch. Wraps at end of display.
static inline int STRIPED_SECTION GetDotX() {
  return pwm_get_counter(VIDEO_PWM);
}

#endif  // VIDEO_DMA_H
