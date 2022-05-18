#ifndef SCAN_OUT_H
#define SCAN_OUT_H

void InitVRAMTest(int bpp, int width);

void RenderLine4(uint32_t* dest, int line, int width);
void RenderLine16(uint32_t* dest, int line, int width);
void RenderLine256(uint32_t* dest, int line, int width);

#endif  // SCAN_OUT_H
