#ifndef SCAN_OUT_H
#define SCAN_OUT_H

#define TILE_BANK_SIZE 65536

int ReadVideoMemByte(int device, int address);
void WriteVideoMemByte(int device, int address, int data);

void ScanOutBeginDisplay();
void ScanOutLine(uint8_t* dest, int y, int width);
void ScanOutEndDisplay();
void InitScanOut();

#endif  // SCAN_OUT_H
