#ifndef FONT5X7_H
#define FONT5X7_H

#include <ppu-types.h>

/*
 * Minimal CPU-side bitmap font renderer: writes pixels directly into a
 * color buffer (no RSX shaders/textures involved). This exists because
 * cgcomp (the Cg shader compiler PSL1GHT's own debugfont_renderer needs)
 * has no working NVIDIA Cg Toolkit on this ARM64 ps3dev build -- Cg was
 * never released for Apple Silicon. Writing straight into the framebuffer
 * from the PPU is PSL1GHT's own documented technique (see rsx.h: "Write
 * the pixel data to the buffer which is not being displayed"), so this
 * sidesteps the shader pipeline entirely.
 *
 * Only covers the characters actually used by Cell-8's intro/menu screens:
 * A-Z, 0-9, '-', '>' (used as a selection marker, not a real character),
 * space. Unknown characters render as blank space.
 */

// Draws `text` (upper-case only) into `buffer` starting at pixel (x, y).
// `pitchPixels` is the buffer's row stride in 32-bit pixels (color_pitch/4).
// `color` is 0x00RRGGBB. `scale` is the pixel size of each font "dot"
// (1 = native 5x7 glyphs, larger values give a bigger, blockier look).
void drawText5x7(u32 *buffer, u32 pitchPixels, s32 x, s32 y,
                  const char *text, u32 color, u32 scale);

// Width in pixels that drawText5x7() would need to render `text` at the
// given scale -- useful for centering text on screen.
s32 textWidth5x7(const char *text, u32 scale);

// Same glyph data as drawText5x7(), but sets cells directly in a
// screenDraw()-style grid buffer (see screen.h) instead of writing pixels
// into a framebuffer -- one grid cell per font "dot", no `scale` (the grid
// is already low-resolution, e.g. 64x32 for the CHIP-8 display). Cells
// outside [0,gridWidth)x[0,gridHeight) are silently skipped. Text is
// UPPER-CASE only, same limited character set as drawText5x7().
void drawText5x7ToGrid(u8 *grid, u32 gridWidth, u32 gridHeight, s32 x, s32 y, const char *text);

#endif // FONT5X7_H
