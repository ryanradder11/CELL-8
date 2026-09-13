#ifndef SCREEN_H
#define SCREEN_H

#include <ppu-types.h>

#define SCREEN_MAX_COLS 240
#define SCREEN_MAX_ROWS 120

void screenInit();
void screenDraw(const u8 *pixels, u32 gridCols, u32 gridRows, u32 gapPixels, u32 maxLevel = 1);

#endif // SCREEN_H
