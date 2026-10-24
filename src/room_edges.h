#ifndef ROOM_EDGES_H
#define ROOM_EDGES_H

#include <stdbool.h>

/* Room transitions at the 16:9 screen edges and the view that goes with
 * them (room_edges.c). */

/* Before each guest frame: patches the game once, then writes the limits of
 * the current view. */
void RoomEdgesApply(void);
/* After each guest frame: logs room changes (FLASHBACK_ROOM_LOG). */
void RoomEdgesAfterRunFrame(void);

/* The view during gameplay: its left edge in room x and its width in
 * columns. False when the transitions are not patched (centred view). */
bool RoomEdgesView(int *left, int *width);

/* Speculative frames use the centred view's limits: true before one, false
 * after. An object leaves its room right at x >= RoomEdgesRightLimit() and
 * left at x < RoomEdgesLeftLimit() with that view. */
void RoomEdgesSpeculativeLimits(bool on);
int RoomEdgesRightLimit(void);
int RoomEdgesLeftLimit(void);

#endif /* ROOM_EDGES_H */
