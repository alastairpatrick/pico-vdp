#ifndef MODES_H
#define MODES_H

void InitVRAMTest();

void RenderLine4(uint32_t* dest, int line, int width);
void RenderLine16(uint32_t* dest, int line, int width);
void RenderLine256(uint32_t* dest, int line, int width);

#endif  // MODES_H