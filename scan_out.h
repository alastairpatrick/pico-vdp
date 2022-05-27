#ifndef SCAN_OUT_H
#define SCAN_OUT_H

#define DISPLAY_BANK_SIZE (32 * 1024 / sizeof(uint32_t))

typedef enum {
  DISPLAY_MODE_DISABLED,
  DISPLAY_MODE_HIRES_2,
  DISPLAY_MODE_HIRES_4,
  DISPLAY_MODE_HIRES_16,
  DISPLAY_MODE_LORES_2,
  DISPLAY_MODE_LORES_4,
  DISPLAY_MODE_LORES_16,
  DISPLAY_MODE_LORES_256,
  DISPLAY_MODE_LORES_TEXT = 14,
  DISPLAY_MODE_HIRES_TEXT,
} DisplayMode;

typedef struct {
  // WORD #0

  // Address/4 of palette for this scanline.
  uint16_t palette_addr: 13;
  int reserved0: 3;

  // Address/4 of of first pixel.
  uint16_t pixels_addr: 13;
  int reserved1: 3;

  // WORD #1

  DisplayMode display_mode: 4;
  bool pixels_addr_en: 1;
  bool display_mode_en: 1;
  bool next_line_en: 1;
  uint8_t reserved3: 1;

  // Which color-quads in palette to update on this scanline.
  uint8_t palette_mask: 4;
  uint8_t reserved2: 4;

  int8_t x_shift: 4;
  bool x_shift_en: 1;
  int8_t reserved4: 3;

  uint8_t reserved5: 8;
} ScanLine;

static_assert(sizeof(ScanLine) == 8);

typedef union {
  ScanLine lines[DISPLAY_BANK_SIZE/2];
  uint32_t words[DISPLAY_BANK_SIZE];
  uint8_t bytes[DISPLAY_BANK_SIZE * sizeof(uint32_t)];
} DisplayBank;

static_assert(sizeof(DisplayBank) == DISPLAY_BANK_SIZE * sizeof(uint32_t));

typedef enum {
  SWAP_SCAN_A_BLIT_A,
  SWAP_SCAN_B_BLIT_B,
  SWAP_SCAN_A_BLIT_B,
  SWAP_SCAN_B_BLIT_A,
  SWAP_MASK = 0x3
} SwapMode;

bool IsBlitClockEnabled();

void SwapBanks(SwapMode mode);
bool IsSwapPending();
DisplayBank* GetBlitBank();

int GetDisplayModeBPP(DisplayMode mode);

void InitScanOutTest(DisplayMode mode, int width, int height);

void ScanOutBeginDisplay();
void ScanOutLine(uint8_t* dest, int y, int width);
void ScanOutEndDisplay();

#endif  // SCAN_OUT_H
