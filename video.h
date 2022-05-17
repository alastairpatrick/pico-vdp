#ifndef VIDEO_H
#define VIDEO_H

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

  VideoAxisTiming horizontal;
  VideoAxisTiming vertical;
} VideoTiming;

typedef void (*VideoLineRenderer)(uint32_t* dest, int line, int width);

extern const VideoTiming g_timing640_480;
extern const VideoTiming g_timing800_600;
extern const VideoTiming g_timing1024_768;

void InitVideo(const VideoTiming* timing, int horizontal_repetitions, int vertical_repetitions, VideoLineRenderer renderer);

#endif  // VIDEO_H
