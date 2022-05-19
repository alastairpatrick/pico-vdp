#ifndef SCAN_OUT_H
#define SCAN_OUT_H

typedef enum {
  DISPLAY_MODE_DISABLED,
  DISPLAY_MODE_HIRES_2,
  DISPLAY_MODE_HIRES_4,
  DISPLAY_MODE_HIRES_16,
  DISPLAY_MODE_LORES_2,
  DISPLAY_MODE_LORES_4,
  DISPLAY_MODE_LORES_16,
  DISPLAY_MODE_LORES_256,
} DisplayMode;

int GetDisplayModeBPP(DisplayMode mode);
void InitScanOutTest(DisplayMode mode, int width, int height);
void ScanOutReset();
void ScanOutLine(uint8_t* dest, int y, int width);

#endif  // SCAN_OUT_H
