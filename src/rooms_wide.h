#ifndef ROOMS_WIDE_H
#define ROOMS_WIDE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Gameplay widescreen: cached neighbouring rooms in the side areas. See
 * rooms_wide.c. */

void RoomsWideBeforeRunFrame(void);
void RoomsWideAfterRunFrame(void);
void RoomsWideOnReset(void);
/* Gameplay is on screen (a room, display on, mode 1). */
bool RoomsWideGameplay(void);
/* Draw the neighbours' facing edges into the side areas of the canvas
 * (outside the field at field_x .. field_x + field_w). False if nothing was
 * drawn; the caller has already cleared the canvas. */
bool RoomsWideDraw(uint8_t *dst, size_t pitch, int canvas_w, int canvas_h,
                   int field_x, int field_w);

/* Development aid: room numbers on screen (FLASHBACK_ROOM_LABELS=1). Drawn
 * last, over the field. */
void RoomsWideDrawLabels(uint8_t *dst, size_t pitch, int canvas_w, int canvas_h,
                         int field_x, int field_w);

#endif /* ROOMS_WIDE_H */
