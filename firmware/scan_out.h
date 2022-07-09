#ifndef SCAN_OUT_H
#define SCAN_OUT_H

#define TILE_BANK_SIZE 65536

typedef enum {
  PLANE_MODE_TEXT_80_30,
  PLANE_MODE_TEXT_80_24,
  PLANE_MODE_TILE,
} PlaneMode;

void ScanOutBeginDisplay();
void ScanOutLine(uint8_t* dest, int y, int width);
void ScanOutEndDisplay();
void InitScanOut();

#endif  // SCAN_OUT_H
