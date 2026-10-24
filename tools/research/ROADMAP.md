# Ideas on hold and their order

## Order

1. Finish neighbour capture (gameplay widescreen Stage B: speculative
   rendering of unvisited neighbours, `src/rooms_wide.c`).
2. Camera pan (below).
3. Shorten the room-change pause.
4. Only then consider parallax art.

## Camera follow (on hold)

Pan the 16:9 view with Conrad across the current room and its cached
neighbour snapshots, so joined rooms read as one longer scene.

Limits:
- Neighbours are snapshots, not live.
- The room switch still has its roughly 1.7 s load pause. Possible
  mitigation: keep the strip visible during the load, or fast-forward the
  load while the screen is blanked.

## Parallax (on hold)

Three layers per showcase room:
1. An optional close-to-lens foreground decoration (new art).
2. The live gameplay layer, which never moves.
3. A far-background image per room (new art), with the room painting's sky
   area masked.

The extra layers move with Conrad's x (`$7E:C740`) and are loaded from the
HD pack by room number (`$1E72`). The work is mostly art; pick a room with a
large open sky area first.

## Reference: room engine facts

- Current room `$1E72`; links at `$7E:A280` up, `$A2C0` down, `$A300`
  right, `$A340` left (+room; `$FF`/`$00` none). Room 216 lines high.
- Conrad's object record `$7E:C73E`: x +2, y +4, room +7. The room change
  test is at `$C26C78`; the room loader is `$C25D3D`.
- A room load takes about 100 frames, mostly with the screen blanked.
- Room transition line (src/room_edges.c): the room-change limits are the
  immediates at `$C26C80` (left, `$FFF7` = -9) and `$C26C97` (right,
  `$0100`). The OAM builder (nine identical blocks in bank `$C2`, first at
  `$C24EEB`) drops pieces outside x -32..255; sprite records live at
  `$7F:0000`, `$7F:0400`, `$7F:0C00`, the OAM shadow at `$7E:BF80`.
- Collision grid: `$7E:A380 + room * $70 + row * 16 + column`, 7 rows of 16;
  rows 0 and 7+ go to the room above/below. `$C27977` (col_getGridData)
  takes the column from `$01E3` (signed, set at `$C268CC`) plus dx and
  follows the left/right links for columns -16..31, so objects past a
  room's edge collide with the neighbour's grid.
- Neighbour objects: `$C26C3E` hands objects in the four linked rooms to
  `$C22361` (sound/message triggers).
- Bank `$C2` has no free space; `$C22612` and `$C2628D` look empty but are
  tables. The HiROM mirrors `$77-$7C:17ED` are read, so the ROM must not be
  extended.
