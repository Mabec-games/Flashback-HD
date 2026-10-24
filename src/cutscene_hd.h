#ifndef CUTSCENE_HD_H
#define CUTSCENE_HD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* HD cutscenes: the polygons the game rasterizes for its cinematics are
 * captured as they are drawn and redrawn at presentation resolution. See
 * cutscene_hd.c and tools/research/CUTSCENE_RENDERER.md. */

void CutsceneHdOnRomLoaded(const uint8_t *rom, size_t size);
void CutsceneHdOnReset(void);
/* Before each guest frame: cutscene pacing (see cutscene_hd.c). */
void CutsceneHdBeforeRunFrame(void);
/* The cinematic engine's layer setup is on screen. */
bool CutsceneHdLayersOnScreen(void);
/* A cutscene is on screen and both of its layers were captured. */
bool CutsceneHdActive(void);
/* Redraw the cutscene's 240x128 viewport into the w x h rectangle at (x0, y0)
 * of an ARGB frame, from the 256-wide PPU field. False if none is shown. */
bool CutsceneHdDraw(uint8_t *dst, size_t pitch, int x0, int y0, int w, int h,
                    const uint8_t *field);

#endif /* CUTSCENE_HD_H */
