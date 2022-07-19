#ifndef SCAN_OUT_H
#define SCAN_OUT_H

#define LORES_DISPLAY_WIDTH 320
#define HIRES_DISPLAY_WIDTH (LORES_DISPLAY_WIDTH / 2)
#define DISPLAY_HEIGHT 240

int ReadVideoMemByte(int device, int address);
void WriteVideoMemByte(int device, int address, int data);

void ScanOutBeginDisplay();
void ScanOutLine(uint8_t* dest, int y, int width);
void ScanOutEndDisplay();
void InitScanOut();

#endif  // SCAN_OUT_H
