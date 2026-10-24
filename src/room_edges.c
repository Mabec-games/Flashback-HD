/* Room transitions at the edges of the 16:9 screen.
 *
 * The game changes room when an object's x leaves -9..255, the edges of the
 * SNES field (the test at $C26C78), moving it 256 pixels into the linked
 * room. With the side areas showing the neighbouring rooms, that line sat
 * inside the 16:9 picture. The view (256 + 2 * side columns, centred on the
 * room) stays put; the game's limits move to its edges, so an object changes
 * room exactly where it walks off the screen. Conrad is then put just
 * inside the screen edge he came in from, as on a flip screen.
 *
 * The limits are written before every frame from the view's position, so a
 * view that moves would only need to set g_view.
 *
 * Collision needs nothing: the game looks up blocks past a room's edge in
 * the neighbouring room (columns -16..31, which bounds the window).
 *
 * Patches, applied to the ROM in memory:
 *   - the room-change test compares x signed (biased by $8000), so the
 *     limits can sit anywhere; they are rewritten before every frame
 *   - the OAM builder (nine copies of one block in bank $C2, e.g. $C24EEB)
 *     drops sprite pieces outside x -32..255; each block's x test is
 *     rewritten in its own 40 bytes to keep pieces across the window, with
 *     the OAM high x bit set right of 256 as it is for negative x. The PPU
 *     hides those; rooms_wide.c draws them.
 * Speculative frames (rooms_wide.c) run with the centred window's limits.
 *
 * Environment:
 *   FLASHBACK_ROOM_EDGES=0       keep the original transition line and a
 *                                centred view (the game is left untouched)
 *   FLASHBACK_ROOM_LOG=1|<file>  log Conrad's position at each room change,
 *                                to the terminal (1) or appended to a file
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common_rtl.h"
#include "cpu_state.h"
#include "present.h"
#include "room_edges.h"
#include "snes/cart.h"
#include "snes/snes.h"

extern Snes *g_snes;

enum {
  kFieldW = 256,
  kTestLeft = 0x26C7D,   /* BPL; CMP #$FFF7; BCS; CLC; ADC #$0100 */
  kTestRight = 0x26C96,  /* CMP #$0100; BMI; BCC; SEC; SBC #$0100 */
  kMaxClipBlocks = 16,
  kConradRecord = 0xC73E,  /* x at +2, y at +4, room at +7 */
  kBodyLeft = 9,  /* the shipped left limit is -9 */
  /* Conrad's sprite covers about x - 9 .. x + 22; where he is put on entry. */
  kEntryFromLeft = kBodyLeft + 4,
  kEntryFromRight = 23 + 4,
  kRightTable = 0xA300,
  kLeftTable = 0xA340,
};

/* ---- The room-change test --------------------------------------------- */

static const uint8_t kTestLeftShipped[] = {
    0x10, 0x17, 0xC9, 0xF7, 0xFF, 0xB0, 0x12, 0x18, 0x69, 0x00, 0x01, 0x97, 0x28,
};
static const uint8_t kTestRightShipped[] = {
    0xC9, 0x00, 0x01, 0x30, 0x14, 0x90, 0x12, 0x38, 0xE9, 0x00, 0x01, 0x97, 0x28,
};

/* A = x after LDA [$28],y. Rewritten:
 *   $C26C7D  EOR #$8000          bias: unsigned compares are now signed
 *   $C26C80  CMP #left^$8000     BCS right test
 *   $C26C85  ADC #$8100          carry is clear: x + 256, unbiased
 *   $C26C96  CMP #right^$8000    BCC no change
 *   $C26C9D  SEC  SBC #$8100     x - 256, unbiased */
static void PatchTest(uint8_t *rom) {
  static const uint8_t left[] = {0x49, 0x00, 0x80, 0xC9, 0xF7, 0x7F, 0xB0, 0x11,
                                 0x69, 0x00, 0x81};
  memcpy(rom + kTestLeft, left, sizeof left);
  rom[kTestRight + 3] = 0x90;  /* BMI -> BCC, same target */
  rom[kTestRight + 10] = 0x81;
}

/* ---- The OAM builder's x clip ------------------------------------------- */

/* As shipped: LDA [$20],y, then 40 bytes from BPL to the STA $66 after. */
static const uint8_t kClipBlock[] = {
    0xB7, 0x20, 0x10, 0x21, 0xC9, 0xE0, 0xFF, 0xB0, 0x0B, 0xC8, 0xC8,
    0xC8, 0xC8, 0xC8, 0xC8, 0xC8, 0xC8, 0x82, 0x71, 0x00, 0x48, 0xA9,
    0x55, 0x55, 0x85, 0x66, 0xA9, 0x55, 0x55, 0x85, 0x68, 0x68, 0x29,
    0xFF, 0x00, 0x80, 0x05, 0xC9, 0x00, 0x01, 0xB0, 0xDF, 0x85, 0x66,
};

/* Its replacement (A = x, 16-bit; dp $68 = 0, the high x bits):
 *   +0  BMI neg
 *   +2  CMP #right   BCS skip      past the right limit
 *   +7  CMP #$0100   BCC keep      on the field
 *  +12  high: PHA  LDA #$5555  STA $68  PLA  AND #$00FF  BRA keep
 *  +24  neg: CMP #left  BCS high
 *  +29  skip: INY x8  BRL next     (the original skip)
 *  +40  keep: STA $66
 * The limits sit at +3 and +25 (from the block's LDA, +5 and +27). */
static void WriteClipBlock(uint8_t *p) {
  static const uint8_t code[40] = {
      0x30, 22,
      0xC9, 0x00, 0x01, 0xB0, 22,
      0xC9, 0x00, 0x01, 0x90, 28,
      0x48, 0xA9, 0x55, 0x55, 0x85, 0x68, 0x68, 0x29, 0xFF, 0x00, 0x80, 16,
      0xC9, 0xE0, 0xFF, 0xB0, (uint8_t)-17,
      0xC8, 0xC8, 0xC8, 0xC8, 0xC8, 0xC8, 0xC8, 0xC8,
      0x82, 0x5B, 0x00,  /* the original BRL's target, from 13 bytes on */
  };
  memcpy(p + 2, code, sizeof code);
}

/* ---- State -------------------------------------------------------------- */

static int g_state = -1;  /* -1 not yet looked at, 0 off, 1 patched */
static int g_clip_block[kMaxClipBlocks], g_clip_blocks;
static int g_side, g_view_w;  /* side columns; window width */
static int g_view = 0;        /* window's left edge, in room x */

static int Centre(void) { return -g_side; }

static void Put(int off, int v) {
  uint8_t *rom = g_snes->cart->rom;
  rom[off] = (uint8_t)v;
  rom[off + 1] = (uint8_t)((unsigned)v >> 8);
}

/* The game's limits for a window starting at view. */
static void WriteLimits(int view) {
  if (g_state != 1)
    return;
  const int left = view - kBodyLeft, right = view + g_view_w;
  Put(kTestLeft + 4, (left ^ 0x8000) & 0xFFFF);
  Put(kTestRight + 1, (right ^ 0x8000) & 0xFFFF);
  /* Sprite pieces across the window and a margin, within the 9-bit OAM x:
   * negative limits only on the left, 256 or more on the right. */
  int clip_left = view - 64, clip_right = view + g_view_w + 32;
  if (clip_left > -1) clip_left = -1;
  if (clip_right < kFieldW) clip_right = kFieldW;
  if (clip_right - clip_left > 512) clip_left = clip_right - 512;
  for (int i = 0; i < g_clip_blocks; i++) {
    Put(g_clip_block[i] + 5, clip_right & 0xFFFF);
    Put(g_clip_block[i] + 27, clip_left & 0xFFFF);
  }
}

void RoomEdgesSpeculativeLimits(bool on) {
  WriteLimits(on ? Centre() : g_view);
}

int RoomEdgesRightLimit(void) { return g_state == 1 ? Centre() + g_view_w : kFieldW; }
int RoomEdgesLeftLimit(void) { return g_state == 1 ? Centre() - kBodyLeft : -kBodyLeft; }

bool RoomEdgesView(int *left, int *width) {
  if (g_state != 1)
    return false;
  *left = g_view;
  *width = g_view_w;
  return true;
}

static bool Matches(const uint8_t *rom, int off, const uint8_t *bytes, size_t n) {
  return !memcmp(rom + off, bytes, n);
}

static void Patch(void) {
  g_state = 0;
  const char *v = getenv("FLASHBACK_ROOM_EDGES");
  if (v && v[0] == '0')
    return;
  uint8_t *rom = g_snes->cart->rom;
  if (g_snes->cart->romSize < 0x30000 ||
      !Matches(rom, kTestLeft, kTestLeftShipped, sizeof kTestLeftShipped) ||
      !Matches(rom, kTestRight, kTestRightShipped, sizeof kTestRightShipped)) {
    fprintf(stderr, "[room_edges] unexpected code at $C26C78, not patched\n");
    return;
  }
  for (int off = 0x20000; off + (int)sizeof kClipBlock <= 0x30000 &&
                          g_clip_blocks < kMaxClipBlocks; off++)
    if (Matches(rom, off, kClipBlock, sizeof kClipBlock)) {
      WriteClipBlock(rom + off);
      g_clip_block[g_clip_blocks++] = off;
    }
  PatchTest(rom);
  g_side = FlashbackSideColumns();
  g_view_w = kFieldW + 2 * g_side;
  g_view = Centre();
  g_state = 1;
  fprintf(stderr, "[room_edges] rooms change at the edges of a %d-column view; "
          "%d sprite clips widened\n", g_view_w, g_clip_blocks);
}

void RoomEdgesApply(void) {
  if (g_state < 0) {
    if (!g_snes || !g_snes->cart || !g_snes->cart->rom)
      return;
    Patch();
  }
  WriteLimits(g_view);
}

static int16_t Word(uint16_t addr) {
  return (int16_t)(g_cpu.ram[addr] | g_cpu.ram[addr + 1] << 8);
}

static void SetWord(uint16_t addr, int v) {
  g_cpu.ram[addr] = (uint8_t)v;
  g_cpu.ram[addr + 1] = (uint8_t)((unsigned)v >> 8);
}

/* Conrad has just changed room sideways: put him inside the screen edge he
 * came in from. */
static void PlaceAtEntryEdge(int from, int to) {
  if (g_state != 1)
    return;
  if (to == g_cpu.ram[kRightTable + from])
    SetWord(kConradRecord + 2, g_view - kBodyLeft + kEntryFromLeft);
  else if (to == g_cpu.ram[kLeftTable + from])
    SetWord(kConradRecord + 2, g_view + g_view_w - kEntryFromRight);
}

/* ---- Room change log ------------------------------------------------------ */

static void LogRoomChange(int from, int to, int16_t x0, int16_t y0, int16_t x,
                          int16_t y) {
  static int enabled = -1;
  static FILE *out;
  if (enabled < 0) {
    const char *v = getenv("FLASHBACK_ROOM_LOG");
    enabled = v && v[0] && v[0] != '0';
    if (enabled && strcmp(v, "1") != 0)
      out = fopen(v, "a");
    if (!out)
      out = stderr;
  }
  if (!enabled)
    return;
  int view = 0, width = kFieldW;
  if (!RoomEdgesView(&view, &width))
    view = 0;
  fprintf(out, "[room] frame %d: room %02X -> %02X, Conrad x %d y %d before, "
          "x %d y %d after; view x %d..%d\n", snes_frame_counter, from, to, x0, y0,
          x, y, view, view + width - 1);
  fflush(out);
}

void RoomEdgesAfterRunFrame(void) {
  static int last_room = -1;
  static int16_t last_x, last_y;
  const int room = g_cpu.ram[kConradRecord + 7];
  if (last_room >= 0 && room != last_room)
    PlaceAtEntryEdge(last_room, room);
  const int16_t x = Word(kConradRecord + 2), y = Word(kConradRecord + 4);
  if (last_room >= 0 && room != last_room)
    LogRoomChange(last_room, room, last_x, last_y, x, y);
  last_room = room;
  last_x = x;
  last_y = y;
}

