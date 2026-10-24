/* Gameplay widescreen: neighbouring rooms in the side areas.
 *
 * Flashback shows one 256-pixel-wide room at a time. Its level map is a grid
 * of rooms with link tables in WRAM (tools/research/ROADMAP.md, "Reference"):
 *   $1E72          current room
 *   $7E:A280+room  room above     $7E:A2C0+room  room below
 *   $7E:A300+room  room right     $7E:A340+room  room left   ($FF/$00 none)
 *
 * Whenever a room is fully on screen, its background (BG1 over BG2, mode 1,
 * decoded from VRAM; sprites such as Conrad, enemies and the HUD excluded)
 * is cached, keyed by the level (a hash of its link tables) and room. While
 * a cached room is the one on screen, the 16:9 side areas show the facing
 * edges of its left and right neighbours from the cache, at the live
 * brightness.
 *
 * A neighbour not visited yet is rendered by the game itself, speculatively:
 * from a snapshot of the machine, Conrad is moved just past that edge of the
 * room and the guest runs until the neighbour has loaded and faded in, one
 * speculative frame before each real frame (rendering off, audio discarded), with
 * the real machine snapshotted and restored around each. Room loads take
 * about 100 frames, so a neighbour is ready about two seconds after entering
 * a room, without stalling play. The real game never sees any of it.
 *
 * Sprites the PPU clips at the field edges (Conrad or an enemy walking out
 * of the room) are drawn on in the side areas, over the neighbour.
 *
 * Neighbouring rooms are spaced exactly one field apart, as the game's own
 * room change moves objects by 256 pixels. Where a seam looks broken, the
 * two rooms' art really does not continue (level 1, rooms 1A and 1B); such
 * joins are blended (see "Seam blending").
 *
 * Environment:
 *   FLASHBACK_WIDE_ROOMS=0   keep plain side bars during gameplay
 *   FLASHBACK_WIDE_DEBUG=1   log room caching and speculation
 *   FLASHBACK_WIDE_DUMP=dir  write each cached room to dir as a PPM
 *   FLASHBACK_SEAM_BLEND=0   show every join as painted, unblended
 *   FLASHBACK_ROOM_LABELS=1  show room numbers (development aid)
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

#include "common_rtl.h"
#include "cpu_state.h"
#include "present.h"
#include "room_edges.h"
#include "rooms_wide.h"
#include "cutscene_hd.h"
#include "snes/ppu.h"
#include "snes/snes.h"
#include "desktop/sdl_compat.h"

extern Snes *g_snes;  /* the runner's machine; no header declares it */

static const double kSpecBudgetMs = 10.0, kSpecBudgetLoadMs = 6.0;

enum {
  kFieldW = 256,
  kFieldH = 224,
  kRoomVar = 0x1E72,
  kLinkTables = 0xA300 - 0x80,  /* WRAM offset of the "up" table, $7E:A280 */
  kRightTable = 0xA300,
  kLeftTable = 0xA340,
  kMaxRooms = 256,
  kStableFrames = 8,
  /* Animated tiles and palette cycling change a few pixels of a room every
   * frame, so rooms are compared with a tolerance, not exactly. */
  kSteadyPixels = kFieldW * kFieldH / 100,   /* frame to frame: 1% */
  kMatchPixels = kFieldW * kFieldH * 3 / 100, /* live vs cached: 3% */

  /* Conrad's object record: x at +2, room at +7 (the engine's room-change
   * test at $C26C78 moves an object whose x leaves -9..255). */
  kConradRecord = 0xC73E,
  /* Conrad is placed this far past the room-change limits (room_edges.c). */
  kSpecPastLimit = 5,
  kSpecDepth = 2,       /* neighbours, and the rooms beyond them */
  kSpecMaxBatch = 12,   /* speculative frames per real frame, at most */
  kSpecMaxFrames = 300,
  kSpecStableFrames = 2,
};

typedef struct Room {
  uint32_t level;
  uint8_t room;
  uint32_t *px;  /* kFieldW x kFieldH, ARGB at full brightness; 0 = backdrop */
} Room;

static Room g_rooms[kMaxRooms];
static int g_room_count, g_next_evict;
static uint32_t g_live[kFieldW * kFieldH], g_prev[kFieldW * kFieldH];
static int g_stable_frames, g_stable_room = -1;
static uint64_t g_real_frame_start;  /* debug timing of the real frame */
static bool g_live_matches_cache;  /* the room on screen is its cached image */
static int g_enabled = -1;
static int g_seam_blend = -1;
static int g_debug = -1;

static bool Debug(void) {
  if (g_debug < 0) {
    const char *v = getenv("FLASHBACK_WIDE_DEBUG");
    g_debug = v && v[0] && v[0] != '0';
  }
  return g_debug;
}

/* ---- BG decode ------------------------------------------------------------ */

static inline uint32_t Rgb15(uint16_t c) {
  const uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
  return 0xff000000u | ((r << 3) | (r >> 2)) << 16 | ((g << 3) | (g >> 2)) << 8 |
         ((b << 3) | (b >> 2));
}

/* One 4bpp layer pixel: colour index (0 = transparent), palette, priority. */
static inline int Bg4bppPixel(const Ppu *ppu, int layer, int sx, int sy, int *pal,
                              int *prio) {
  const int tilemap = PPU_bgTilemapAdr(ppu, layer), tiles = PPU_bgTileAdr(ppu, layer);
  const int wide = PPU_bgTilemapWider(ppu, layer) ? 1 : 0;
  const int high = PPU_bgTilemapHigher(ppu, layer) ? 1 : 0;
  /* A BG line shows map row y + vscroll + 1. */
  const int mx = (sx + ppu->hScroll[layer]) & (wide ? 511 : 255);
  const int my = (sy + ppu->vScroll[layer] + 1) & (high ? 511 : 255);
  int screen = 0;
  if (mx >= 256) screen += 1;
  if (my >= 256) screen += wide ? 2 : 1;
  const uint16_t e = ppu->vram[(tilemap + screen * 0x400 + ((my & 255) >> 3) * 32 +
                                ((mx & 255) >> 3)) & 0x7fff];
  int tx = mx & 7, ty = my & 7;
  if (e & 0x4000) tx = 7 - tx;
  if (e & 0x8000) ty = 7 - ty;
  const int base = tiles + (e & 0x3ff) * 16;
  const uint16_t w0 = ppu->vram[(base + ty) & 0x7fff];
  const uint16_t w1 = ppu->vram[(base + 8 + ty) & 0x7fff];
  const int b = 7 - tx;
  *pal = (e >> 10) & 7;
  *prio = (e >> 13) & 1;
  return ((w0 >> b) & 1) | (((w0 >> (8 + b)) & 1) << 1) | (((w1 >> b) & 1) << 2) |
         (((w1 >> (8 + b)) & 1) << 3);
}

/* A room pixel from a high-priority tile has the low bit of its alpha clear,
 * so sprites drawn over a cached room (DrawSide) keep their order against
 * the background. */
enum { kLowPriorityBit = 0x01000000u };

/* BG1 and BG2 of mode 1 in priority order (BG1 high, BG2 high, BG1 low, BG2
 * low); 0 where both are transparent. */
static void DecodeRoom(const Ppu *ppu, uint32_t *out) {
  for (int y = 0; y < kFieldH; y++)
    for (int x = 0; x < kFieldW; x++) {
      int p1, q1, p2, q2;
      const int c1 = Bg4bppPixel(ppu, 0, x, y, &p1, &q1);
      const int c2 = Bg4bppPixel(ppu, 1, x, y, &p2, &q2);
      uint32_t c = 0;
      if (c1 && q1) c = Rgb15(ppu->cgram[p1 * 16 + c1]) & ~kLowPriorityBit;
      else if (c2 && q2) c = Rgb15(ppu->cgram[p2 * 16 + c2]) & ~kLowPriorityBit;
      else if (c1) c = Rgb15(ppu->cgram[p1 * 16 + c1]);
      else if (c2) c = Rgb15(ppu->cgram[p2 * 16 + c2]);
      out[y * kFieldW + x] = c;
    }
}

static int16_t Word(uint16_t addr) {
  return (int16_t)(g_cpu.ram[addr] | g_cpu.ram[addr + 1] << 8);
}

/* ---- Room cache ------------------------------------------------------------ */

static uint32_t Hash(const uint8_t *p, size_t n, uint32_t h) {
  for (size_t i = 0; i < n; i++)
    h = (h ^ p[i]) * 0x01000193u;
  return h;
}

static uint32_t LevelKey(void) {
  return Hash(g_cpu.ram + kLinkTables, 0x100, 0x811c9dc5u);
}

static Room *StoreRoomInMemory(uint32_t level, int room, const uint32_t *px);

/* Captured rooms are also kept on disk (saves/rooms/<level>_<room>.bin, the
 * decoded pixels), so a room seen in an earlier session has its picture at
 * once, the first room of a level included. Derived from the player's own
 * ROM and never shipped. */
static void RoomPath(char *path, size_t n, uint32_t level, int room) {
  snprintf(path, n, "saves/rooms/%08X_%02X.bin", level, room);
}

static void MakeDir(const char *path) {
#ifdef _WIN32
  _mkdir(path);
#else
  mkdir(path, 0755);
#endif
}

static void SaveRoomFile(uint32_t level, int room, const uint32_t *px) {
  char path[64];
  MakeDir("saves");
  MakeDir("saves/rooms");
  RoomPath(path, sizeof path, level, room);
  FILE *f = fopen(path, "wb");
  if (!f)
    return;
  fwrite(px, sizeof(uint32_t), kFieldW * kFieldH, f);
  fclose(f);
}

static Room *LoadRoomFile(uint32_t level, int room) {
  static uint8_t tried[0x40];
  static uint32_t tried_level;
  if (level != tried_level) {
    memset(tried, 0, sizeof tried);
    tried_level = level;
  }
  if (room <= 0 || room >= 0x40 || tried[room]++)
    return NULL;
  char path[64];
  RoomPath(path, sizeof path, level, room);
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  static uint32_t px[kFieldW * kFieldH];
  const size_t n = fread(px, sizeof(uint32_t), kFieldW * kFieldH, f);
  fclose(f);
  return n == (size_t)kFieldW * kFieldH ? StoreRoomInMemory(level, room, px) : NULL;
}

static Room *FindRoom(uint32_t level, int room) {
  for (int i = 0; i < g_room_count; i++)
    if (g_rooms[i].level == level && g_rooms[i].room == room)
      return &g_rooms[i];
  return LoadRoomFile(level, room);
}

/* Pixels that differ between two room images, counting no further than
 * limit + 1. */
static int DiffCount(const uint32_t *a, const uint32_t *b, int limit) {
  int n = 0;
  for (int i = 0; i < kFieldW * kFieldH && n <= limit; i++)
    n += a[i] != b[i];
  return n;
}

static Room *StoreRoomInMemory(uint32_t level, int room, const uint32_t *px) {
  Room *r = NULL;
  for (int i = 0; i < g_room_count && !r; i++)
    if (g_rooms[i].level == level && g_rooms[i].room == room)
      r = &g_rooms[i];
  if (!r) {
    if (g_room_count < kMaxRooms) {
      r = &g_rooms[g_room_count++];
    } else {
      r = &g_rooms[g_next_evict];
      g_next_evict = (g_next_evict + 1) % kMaxRooms;
    }
    if (!r->px)
      r->px = (uint32_t *)malloc(sizeof g_live);
    if (!r->px)
      return NULL;
    r->level = level;
    r->room = (uint8_t)room;
  }
  memcpy(r->px, px, sizeof g_live);
  return r;
}

static void StoreRoom(uint32_t level, int room, const uint32_t *px) {
  if (!StoreRoomInMemory(level, room, px))
    return;
  SaveRoomFile(level, room, px);
  if (Debug())
    fprintf(stderr, "[rooms_wide] frame %d: cached room %02X (level %08X)\n",
            snes_frame_counter, room, level);
  const char *dir = getenv("FLASHBACK_WIDE_DUMP");
  if (dir && *dir) {
    char path[512];
    snprintf(path, sizeof path, "%s/room_%08X_%02X.ppm", dir, level, room);
    FILE *f = fopen(path, "wb");
    if (f) {
      fprintf(f, "P6\n%d %d\n255\n", kFieldW, kFieldH);
      for (int i = 0; i < kFieldW * kFieldH; i++) {
        const uint32_t c = px[i];
        fputc((c >> 16) & 255, f); fputc((c >> 8) & 255, f); fputc(c & 255, f);
      }
      fclose(f);
    }
  }
}

/* Gameplay is on screen: mode 1, display on at full brightness, a room
 * number the link tables know. */
static bool GameplayOnScreen(const Ppu *ppu) {
  const int room = g_cpu.ram[kRoomVar];
  return ppu && !PPU_forcedBlank(ppu) && PPU_mode(ppu) == 1 &&
         (ppu->screenEnabled[0] & 3) == 3 && room > 0 && room < 0x40;
}

bool RoomsWideGameplay(void) {
  return GameplayOnScreen(g_ppu);
}

static bool Enabled(void) {
  if (g_enabled < 0) {
    const char *v = getenv("FLASHBACK_WIDE_ROOMS");
    g_enabled = !(v && v[0] == '0');
  }
  return g_enabled;
}

/* After each guest frame: cache the room once it has been steady on screen
 * at full brightness for a few frames (so a fade or a load in progress is
 * never stored). */
void RoomsWideAfterRunFrame(void) {
  const Ppu *ppu = g_ppu;
  if (Debug() && g_real_frame_start) {
    static double total, worst;
    static int n;
    const double ms = (double)(SDL_GetPerformanceCounter() - g_real_frame_start) *
                      1000.0 / (double)SDL_GetPerformanceFrequency();
    total += ms;
    if (ms > worst) worst = ms;
    if (++n % 60 == 0)
      fprintf(stderr, "[rooms_wide]   real frame after speculation: %.2f ms average, "
              "worst %.2f ms\n", total / n, worst);
    g_real_frame_start = 0;
  }
  g_live_matches_cache = false;
  if (!Enabled() || !GameplayOnScreen(ppu)) {
    g_stable_frames = 0;
    return;
  }
  DecodeRoom(ppu, g_live);
  const int changed = DiffCount(g_live, g_prev, kFieldW * kFieldH);
  const bool steady = (ppu->inidisp & 15) == 15 && changed <= kSteadyPixels;
  memcpy(g_prev, g_live, sizeof g_live);
  const uint32_t level = LevelKey();
  const int room = g_cpu.ram[kRoomVar];
  /* Tested on every frame, not just steady ones, so the neighbours fade in
   * with a room that is already cached (the cache is at full brightness and
   * the fade is the INIDISP brightness, which DrawSide applies). */
  const Room *here = FindRoom(level, room);
  g_live_matches_cache =
      here && DiffCount(g_live, here->px, kMatchPixels) <= kMatchPixels;
  if (!steady) {
    g_stable_frames = 0;
    return;
  }
  /* Steadiness belongs to one room: the room number changes as a room load
   * starts, before the screen blanks. */
  if (room != g_stable_room) {
    g_stable_room = room;
    g_stable_frames = 0;
  }
  if (++g_stable_frames == kStableFrames) {
    StoreRoom(level, room, g_live);
    g_live_matches_cache = FindRoom(level, room) != NULL;
  }
  if (Debug() && snes_frame_counter % 60 == 0)
    fprintf(stderr, "[rooms_wide] frame %d: room %02X changed %d px, stable %d, "
            "matches cache %d\n", snes_frame_counter, room, changed, g_stable_frames,
            g_live_matches_cache);
}

/* ---- Presentation ---------------------------------------------------------- */

static inline int FloorDiv(long long a, long long b) {
  return (int)(a >= 0 ? a / b : -((-a + b - 1) / b));
}

/* Sprites past the field edges. The PPU clips objects at the edges of the
 * 256-pixel field; the game's OAM builder, widened by room_edges.c, keeps
 * the pieces of objects out to the edges of the 16:9 screen, which are
 * drawn here in the side areas, over the neighbour. The 9-bit OAM x is read
 * within the 512 columns from just left of the view; the game hides unused
 * slots below the screen (y 224), not at such an x. */
enum { kSideW = 256 };  /* field columns that can be shown beyond each edge */

typedef struct SidePixel {
  uint32_t colour;  /* 0 = no sprite */
  uint8_t prio;
} SidePixel;

static SidePixel g_side_obj[kFieldH][2 * kSideW];
static bool g_side_obj_any;

static SidePixel *SideObj(int fx, int fy) {
  if (fy < 0 || fy >= kFieldH)
    return NULL;
  if (fx < 0 && fx >= -kSideW)
    return &g_side_obj[fy][fx + kSideW];
  if (fx >= kFieldW && fx < kFieldW + kSideW)
    return &g_side_obj[fy][kSideW + fx - kFieldW];
  return NULL;
}

static void BuildSideSprites(const Ppu *ppu) {
  static const uint8_t kSizes[8][2] = {
      {8, 16}, {8, 32}, {8, 64}, {16, 32}, {16, 64}, {32, 64}, {16, 32}, {16, 32},
  };
  memset(g_side_obj, 0, sizeof g_side_obj);
  g_side_obj_any = false;
  int view = -FlashbackSideColumns(), width = 0;
  RoomEdgesView(&view, &width);
  const int view_lo = view - 64;
  /* Lower OAM slots are in front: draw from the back. */
  for (int i = 127; i >= 0; i--) {
    const uint16_t w0 = ppu->oam[i * 2], w1 = ppu->oam[i * 2 + 1];
    const int hi = (ppu->highOam[i >> 2] >> ((i & 3) * 2)) & 3;
    const int size = kSizes[ppu->obsel >> 5][hi >> 1];
    /* The 9-bit x, read within the 512 columns from just left of the view
     * (room_edges.c keeps the game's pieces in that range). */
    int x = (w0 & 255) | (hi & 1) << 8;
    if (x >= view_lo + 512)
      x -= 512;
    else if (x < view_lo)
      x += 512;
    if (x + size <= -kSideW || x >= kFieldW + kSideW ||
        (x >= 0 && x + size <= kFieldW))
      continue;  /* beyond the side areas, or wholly on the field */
    const int y = w0 >> 8;
    const int tile = w1 & 255;
    const int base = (w1 & 0x100) ? PPU_objTileAdr2(ppu) : PPU_objTileAdr1(ppu);
    const int pal = 128 + ((w1 >> 9) & 7) * 16;
    const int prio = (w1 >> 12) & 3;
    for (int row = 0; row < size; row++) {
      const int fy = (y + row) & 255;
      if (fy >= kFieldH)
        continue;
      const int r = (w1 & 0x8000) ? size - 1 - row : row;
      for (int col = 0; col < size; col++) {
        SidePixel *out = SideObj(x + col, fy);
        if (!out)
          continue;
        const int c = (w1 & 0x4000) ? size - 1 - col : col;
        const int t = ((((tile >> 4) + (r >> 3)) & 15) << 4) | (((tile & 15) + (c >> 3)) & 15);
        const int adr = base + t * 16 + (r & 7);
        const uint16_t p0 = ppu->vram[adr & 0x7fff], p1 = ppu->vram[(adr + 8) & 0x7fff];
        const int b = 7 - (c & 7);
        const int index = ((p0 >> b) & 1) | ((p0 >> (8 + b)) & 1) << 1 |
                          ((p1 >> b) & 1) << 2 | ((p1 >> (8 + b)) & 1) << 3;
        if (!index)
          continue;
        out->colour = Rgb15(ppu->cgram[pal + index]);
        out->prio = (uint8_t)prio;
        g_side_obj_any = true;
      }
    }
  }
}

/* ---- Seam blending ----------------------------------------------------------
 *
 * Flashback's rooms are separate paintings: many joins continue (level 1,
 * 1B|1C), some do not (1A|1B, a vine trunk against a mossy block). Each join
 * is scored in bands of rows by comparing the colour of the 4 columns either
 * side of the seam, low-passed over the band, with the variation between
 * neighbouring 4-column blocks on the same side (the art's own texture).
 * Where the seam is clearly rougher than the art, the neighbour's first
 * columns change over, with ordered dithering so it stays pixel art, from a
 * mirror of the room's own edge to the neighbour. Matching bands are left
 * as they are. */
enum {
  kBlendW = 24,        /* field columns the change-over takes */
  kBand = 8,
  kBands = kFieldH / kBand,
  kEdgeW = 4,
  kSeamExcessLo = 40,  /* excess (RGB sum, 0..765) where blending starts */
  kSeamExcessHi = 120, /* and where it is complete */
};

static uint32_t RoomPixel(const Room *r, int x, int y, uint32_t backdrop) {
  const uint32_t p = r->px[y * kFieldW + x];
  return p ? p : backdrop;
}

static void BlockMean(const Room *r, int x0, int y0, uint32_t backdrop, int out[3]) {
  out[0] = out[1] = out[2] = 0;
  for (int y = y0; y < y0 + kBand; y++)
    for (int x = x0; x < x0 + kEdgeW; x++) {
      const uint32_t c = RoomPixel(r, x, y, backdrop);
      out[0] += (c >> 16) & 255;
      out[1] += (c >> 8) & 255;
      out[2] += c & 255;
    }
  for (int i = 0; i < 3; i++)
    out[i] /= kBand * kEdgeW;
}

static int BlockDist(const int a[3], const int b[3]) {
  return abs(a[0] - b[0]) + abs(a[1] - b[1]) + abs(a[2] - b[2]);
}

/* Blend strength per row, 0..256, of the seam between room l (left) and
 * room r (right). */
static void SeamWeights(const Room *l, const Room *r, uint32_t backdrop,
                        int weight[kFieldH]) {
  int band[kBands];
  for (int i = 0; i < kBands; i++) {
    int l1[3], l2[3], r1[3], r2[3];
    BlockMean(l, kFieldW - 2 * kEdgeW, i * kBand, backdrop, l1);
    BlockMean(l, kFieldW - kEdgeW, i * kBand, backdrop, l2);
    BlockMean(r, 0, i * kBand, backdrop, r1);
    BlockMean(r, kEdgeW, i * kBand, backdrop, r2);
    const int excess =
        BlockDist(l2, r1) - (BlockDist(l1, l2) + BlockDist(r1, r2)) / 2;
    const int w = (excess - kSeamExcessLo) * 256 / (kSeamExcessHi - kSeamExcessLo);
    band[i] = w < 0 ? 0 : w > 256 ? 256 : w;
  }
  /* Linear between band centres, so strength never steps at a band edge. */
  for (int y = 0; y < kFieldH; y++) {
    const int t = y - kBand / 2;
    const int i = t < 0 ? 0 : t / kBand;
    const int j = i + 1 < kBands ? i + 1 : i;
    const int f = t < 0 ? 0 : t % kBand;
    weight[y] = (band[i] * (kBand - f) + band[j] * f) / kBand;
  }
}

/* 4x4 ordered-dither thresholds, 0..15. */
static const uint8_t kBayer4[4][4] = {
    {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5},
};

/* Fill canvas columns x0..x1 (a side area): the facing edge of room (offset
 * room_dx field pixels from the live one, which is `here`; NULL for none,
 * leaving the canvas black), blended into here's mirrored edge where the
 * seam breaks, with any sprites past that field edge on top. In mode 1, OBJ
 * priority 3 is in front of both BG layers, 2 behind their high-priority
 * tiles, 1 and 0 behind every BG pixel. */
static void DrawSide(uint8_t *dst, size_t pitch, int x0, int x1, int canvas_h,
                     int field_x, int field_w, const Room *room, int room_dx,
                     const Room *here, const Ppu *ppu) {
  const uint32_t backdrop = Rgb15(ppu->cgram[0]);
  const uint32_t bright = (uint32_t)(ppu->inidisp & 15);
  int weight[kFieldH];
  const bool blend = room && here && g_seam_blend;
  if (blend) {
    if (room_dx > 0)
      SeamWeights(here, room, backdrop, weight);
    else
      SeamWeights(room, here, backdrop, weight);
  }
  for (int y = 0; y < canvas_h; y++) {
    uint32_t *row = (uint32_t *)(dst + (size_t)y * pitch);
    const int fy = y * kFieldH / canvas_h;
    for (int x = x0; x < x1; x++) {
      /* Field column of this canvas pixel, continued past the edges. */
      const int fx = FloorDiv((long long)(x - field_x) * kFieldW, field_w);
      const int rx = fx - room_dx;
      uint32_t c = 0;
      bool high = false, have_bg = false;
      if (room && rx >= 0 && rx < kFieldW) {
        uint32_t p = room->px[fy * kFieldW + rx];
        /* Columns from the seam: 0 is the one touching the field. */
        const int k = room_dx > 0 ? rx : kFieldW - 1 - rx;
        if (blend && k < kBlendW && weight[fy]) {
          const int strength = weight[fy] * (2 * (kBlendW - k) - 1) / (2 * kBlendW);
          if (strength * 16 > ((int)kBayer4[fy & 3][fx & 3] * 2 + 1) * 128)
            p = here->px[fy * kFieldW + (room_dx > 0 ? kFieldW - 1 - k : k)];
        }
        have_bg = true;
        c = p ? p : backdrop;
        high = p && !(p & kLowPriorityBit);
      }
      const SidePixel *obj = g_side_obj_any ? SideObj(fx, fy) : NULL;
      if (obj && obj->colour &&
          (!have_bg || obj->prio == 3 || (obj->prio == 2 && !high) ||
           c == backdrop))
        c = obj->colour;
      else if (!have_bg)
        continue;
      if (bright != 15) {
        const uint32_t r = ((c >> 16) & 255) * bright / 15;
        const uint32_t g = ((c >> 8) & 255) * bright / 15;
        const uint32_t b = (c & 255) * bright / 15;
        c = r << 16 | g << 8 | b;
      }
      row[x] = 0xff000000u | c;
    }
  }
}

/* ---- Development room labels -------------------------------------------- */

/* 3x5 glyphs, rows top to bottom, 3 bits each (bit 2 = left column). */
static const uint16_t kGlyph[] = {
    /* 0-9 */
    075557, 026227, 071747, 071717, 055711, 074717, 074757, 071111, 075757, 075717,
    /* A-F */
    025755, 065656, 034443, 065556, 074647, 074644,
};

static int GlyphIndex(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'A' && ch <= 'F') return 10 + ch - 'A';
  return -1;
}

/* Text at canvas (x, y), each glyph pixel a px-by-px square, on a dark box. */
static void DrawText(uint8_t *dst, size_t pitch, int canvas_w, int canvas_h, int x,
                     int y, int px, const char *text, uint32_t colour) {
  const int n = (int)strlen(text);
  const int w = (n * 4 + 1) * px, h = 7 * px;
  for (int yy = y; yy < y + h && yy < canvas_h; yy++)
    for (int xx = x; xx < x + w && xx < canvas_w; xx++)
      if (xx >= 0 && yy >= 0)
        ((uint32_t *)(dst + (size_t)yy * pitch))[xx] = 0xff000000u;
  for (int i = 0; i < n; i++) {
    const int g = GlyphIndex(text[i]);
    if (g < 0)
      continue;
    for (int gy = 0; gy < 5; gy++)
      for (int gx = 0; gx < 3; gx++) {
        if (!((kGlyph[g] >> ((4 - gy) * 3 + (2 - gx))) & 1))
          continue;
        for (int yy = 0; yy < px; yy++)
          for (int xx = 0; xx < px; xx++) {
            const int cx = x + ((i * 4 + 1 + gx) * px) + xx;
            const int cy = y + ((1 + gy) * px) + yy;
            if (cx >= 0 && cx < canvas_w && cy >= 0 && cy < canvas_h)
              ((uint32_t *)(dst + (size_t)cy * pitch))[cx] = colour;
          }
      }
  }
}

static int g_labels = -1;

/* FLASHBACK_ROOM_LABELS=1: the room number (hex, as the game numbers it in
 * $1E72) over the top left of the field, and each neighbour's number in its
 * side area, so rooms can be named in reports. */
void RoomsWideDrawLabels(uint8_t *dst, size_t pitch, int canvas_w, int canvas_h,
                         int field_x, int field_w) {
  if (g_labels < 0) {
    const char *v = getenv("FLASHBACK_ROOM_LABELS");
    g_labels = v && v[0] && v[0] != '0';
  }
  const Ppu *ppu = g_ppu;
  if (!g_labels || !GameplayOnScreen(ppu))
    return;
  const int px = canvas_h / kFieldH * 2 > 1 ? canvas_h / kFieldH * 2 : 2;
  const int room = g_cpu.ram[kRoomVar];
  const int nb[2] = {g_cpu.ram[kLeftTable + room], g_cpu.ram[kRightTable + room]};
  char text[8];
  snprintf(text, sizeof text, "%02X", room);
  DrawText(dst, pitch, canvas_w, canvas_h, field_x + 2 * px, 2 * px, px, text,
           0xffffff40u);
  for (int i = 0; i < 2; i++) {
    if (nb[i] <= 0 || nb[i] >= 0x40)
      continue;
    snprintf(text, sizeof text, "%02X", nb[i]);
    const int w = 9 * px;
    const int x = i == 0 ? field_x - w - 2 * px : field_x + field_w + 2 * px;
    DrawText(dst, pitch, canvas_w, canvas_h, x, 2 * px, px, text, 0xffa0a0a0u);
  }
}

bool RoomsWideDraw(uint8_t *dst, size_t pitch, int canvas_w, int canvas_h,
                   int field_x, int field_w) {
  const Ppu *ppu = g_ppu;
  if (!Enabled() || !GameplayOnScreen(ppu))
    return false;
  if (g_seam_blend < 0) {
    const char *v = getenv("FLASHBACK_SEAM_BLEND");
    g_seam_blend = !(v && v[0] == '0');
  }
  BuildSideSprites(ppu);
  const uint32_t level = LevelKey();
  const int room = g_cpu.ram[kRoomVar];
  /* Neighbours only while the cached image of this room is what the PPU
   * shows: not during a load, a menu over the room, or a room not cached
   * yet. Sprites past the edges are drawn regardless. */
  const Room *left = NULL, *right = NULL, *here = NULL;
  if (g_live_matches_cache) {
    here = FindRoom(level, room);
    left = FindRoom(level, g_cpu.ram[kLeftTable + room]);
    right = FindRoom(level, g_cpu.ram[kRightTable + room]);
  }
  if (!left && !right && !g_side_obj_any)
    return false;
  DrawSide(dst, pitch, 0, field_x, canvas_h, field_x, field_w, left, -kFieldW, here,
           ppu);
  DrawSide(dst, pitch, field_x + field_w, canvas_w, canvas_h, field_x, field_w,
           right, kFieldW, here, ppu);
  return true;
}

/* ---- Speculative neighbour rendering --------------------------------------- */

typedef struct Speculation {
  bool active;
  uint32_t level;
  int source, target;
  int dir;       /* +1 right, -1 left */
  int depth;     /* 1 for a neighbour of the real room, 2 for the next one */
  bool reached;  /* the target room has been entered */
  bool seen;     /* g_spec_px holds it, shown at full brightness */
  bool early;    /* started before the real room was steady on screen */
  int frames, stable;
  size_t len;
} Speculation;

static Speculation g_spec;
static uint8_t *g_real_blob, *g_spec_blob;
static size_t g_blob_cap;
static uint8_t g_failed[0x40];       /* per room, for the current level */
static uint32_t g_failed_level;
static uint32_t g_spec_px[kFieldW * kFieldH], g_spec_prev[kFieldW * kFieldH];

static bool EnsureBlobs(void) {
  const size_t want = RtlRollbackSnapshotBound();
  if (!want)
    return false;
  if (g_blob_cap >= want)
    return true;
  uint8_t *a = (uint8_t *)realloc(g_real_blob, want);
  if (a) g_real_blob = a;
  uint8_t *b = (uint8_t *)realloc(g_spec_blob, want);
  if (b) g_spec_blob = b;
  if (!a || !b)
    return false;
  g_blob_cap = want;
  return true;
}

/* Back to the real machine. The PPU's per-line raster journal lives outside
 * the snapshot and is double-buffered per field: a speculative frame swaps
 * it and seeds the next field from the speculative machine's registers
 * (often in forced blank mid-load), so the next real frame would draw black.
 * Re-arm it, as a machine reset does; the next frame seeds it afresh. */
static void RestoreReal(size_t len) {
  if (!RtlRollbackLoadFromMemory(g_real_blob, len)) {
    fprintf(stderr, "[rooms_wide] restoring the real machine FAILED\n");
    abort();  /* the guest is no longer where the player left it */
  }
  ppu_rasterReset();
}


static void SetWord(uint16_t addr, int16_t v) {
  g_cpu.ram[addr] = (uint8_t)v;
  g_cpu.ram[addr + 1] = (uint8_t)((uint16_t)v >> 8);
}

/* Run one speculative guest frame and leave no trace but its effect on the
 * speculative machine (which the caller saves): no picture, no audio, no
 * frame count, no mod hook. */
static void RunSpeculativeFrame(void) {
  const int counter = snes_frame_counter;
  RoomEdgesSpeculativeLimits(true);
  const uint32_t audio = RtlAudioProducerCursor();
  const bool render = g_snes->disableRender;
  g_snes->disableRender = true;
  RtlSetSpeculativeFrame(true);
  RtlRunFrame(0);
  RtlSetSpeculativeFrame(false);
  g_snes->disableRender = render;
  RtlAudioRewindProducer(audio);
  snes_frame_counter = counter;
  RoomEdgesSpeculativeLimits(false);
}

/* After a speculative frame: has the target room finished loading? */
static bool SpeculationDone(void) {
  const Ppu *ppu = g_ppu;
  if (g_cpu.ram[kRoomVar] != g_spec.target || !GameplayOnScreen(ppu) ||
      (ppu->inidisp & 15) != 15) {
    g_spec.stable = 0;
    return false;
  }
  DecodeRoom(ppu, g_spec_px);
  g_spec.seen = true;
  const bool steady =
      DiffCount(g_spec_px, g_spec_prev, kSteadyPixels) <= kSteadyPixels;
  memcpy(g_spec_prev, g_spec_px, sizeof g_spec_px);
  g_spec.stable = steady ? g_spec.stable + 1 : 0;
  return g_spec.stable >= kSpecStableFrames;
}

/* Choose a missing neighbour of the steady current room and start rendering
 * it, from the real machine as it is now. */
/* Conrad's x just past the room's edge in direction dir (speculative frames
 * use the centred view's limits). */
static int16_t PastEdge(int dir) {
  return (int16_t)(dir > 0 ? RoomEdgesRightLimit() + kSpecPastLimit
                           : RoomEdgesLeftLimit() - 1 - kSpecPastLimit);
}

static bool Wanted(uint32_t level, int room) {
  return room > 0 && room < 0x40 && !g_failed[room] && !FindRoom(level, room);
}

/* Choose a missing neighbour of the steady current room and start rendering
 * it, from the real machine as it is now. */
static void StartSpeculation(uint32_t level, int room) {
  if (level != g_failed_level) {
    memset(g_failed, 0, sizeof g_failed);
    g_failed_level = level;
  }
  /* Only when the player object really is Conrad, in this room. */
  if (g_cpu.ram[kConradRecord + 7] != room)
    return;
  /* A missing neighbour first; then the room beyond a cached one, so the
   * room Conrad walks into already has both sides. */
  int target = -1, dir = 0, depth = 0;
  for (int pass = 1; pass <= kSpecDepth && target < 0; pass++)
    for (int d = 1; d >= -1 && target < 0; d -= 2) {
      const int near = g_cpu.ram[(d > 0 ? kRightTable : kLeftTable) + room];
      if (near <= 0 || near >= 0x40)
        continue;
      if (pass == 1 && Wanted(level, near)) {
        target = near;
      } else if (pass == 2 && FindRoom(level, near)) {
        const int far = g_cpu.ram[(d > 0 ? kRightTable : kLeftTable) + near];
        if (Wanted(level, far))
          target = near;  /* entered on the way to far */
      }
      if (target >= 0) {
        dir = d;
        depth = pass;
      }
    }
  if (target < 0 || !EnsureBlobs())
    return;
  if (Debug())
    fprintf(stderr, "[rooms_wide] frame %d: speculating room %02X (%s of %02X)%s\n",
            snes_frame_counter, target, dir > 0 ? "right" : "left", room,
            depth > 1 ? ", on the way further" : "");
  const size_t real = RtlRollbackSaveToMemory(g_real_blob, g_blob_cap);
  if (!real)
    return;
  SetWord(kConradRecord + 2, PastEdge(dir));
  RunSpeculativeFrame();
  g_spec.len = RtlRollbackSaveToMemory(g_spec_blob, g_blob_cap);
  RestoreReal(real);
  if (!g_spec.len)
    return;
  g_spec.active = true;
  g_spec.level = level;
  g_spec.source = room;
  g_spec.target = target;
  g_spec.dir = dir;
  /* A depth-2 run passes through a cached room: count it as level 1 done. */
  g_spec.depth = 1;
  g_spec.early = g_stable_frames < kStableFrames || g_cpu.ram[kRoomVar] != g_stable_room;
  g_spec.reached = false;
  g_spec.seen = false;
  g_spec.frames = 1;
  g_spec.stable = 0;
}

/* In the speculative machine, with the target just captured: carry on to
 * the room beyond it, if that one is wanted. */
static bool ContinueSpeculation(void) {
  if (g_spec.depth >= kSpecDepth)
    return false;
  /* The source room's other neighbour comes first. */
  const int other =
      g_cpu.ram[(g_spec.dir > 0 ? kLeftTable : kRightTable) + g_spec.source];
  if (g_spec.depth == 1 && Wanted(g_spec.level, other))
    return false;
  const int far =
      g_cpu.ram[(g_spec.dir > 0 ? kRightTable : kLeftTable) + g_spec.target];
  if (!Wanted(g_spec.level, far) || g_cpu.ram[kConradRecord + 7] != g_spec.target)
    return false;
  if (Debug())
    fprintf(stderr, "[rooms_wide]   on to room %02X\n", far);
  SetWord(kConradRecord + 2, PastEdge(g_spec.dir));
  g_spec.source = g_spec.target;
  g_spec.target = far;
  g_spec.depth++;
  g_spec.reached = false;
  g_spec.seen = false;
  g_spec.frames = 0;
  g_spec.stable = 0;
  return true;
}

/* After a speculative frame: true when the run is over. */
static bool AdvanceSpeculation(void) {
  g_spec.frames++;
  if (Debug() && g_spec.frames % 30 == 0)
    fprintf(stderr, "[rooms_wide]   speculative frame %d: room %02X inidisp %02X "
            "conrad x %d y %d\n", g_spec.frames, g_cpu.ram[kRoomVar], g_ppu->inidisp,
            Word(kConradRecord + 2), Word(kConradRecord + 4));
  const int now = g_cpu.ram[kRoomVar];
  g_spec.reached |= now == g_spec.target;
  /* Gone anywhere but the target, after leaving the source: Conrad fell on
   * through it (he is dropped in at the height he had, which may have no
   * floor there) or died on the way in (a pit, a trap). If the room was
   * shown at full brightness first, that picture is kept; otherwise there
   * is nothing to capture. */
  const bool lost = now != g_spec.target && (g_spec.reached || now != g_spec.source);
  if (lost && g_spec.seen) {
    if (!FindRoom(g_spec.level, g_spec.target))
      StoreRoom(g_spec.level, g_spec.target, g_spec_px);
    return true;
  }
  if (!lost && SpeculationDone()) {
    if (!FindRoom(g_spec.level, g_spec.target))
      StoreRoom(g_spec.level, g_spec.target, g_spec_px);
    return !ContinueSpeculation();
  }
  if (lost || g_spec.frames >= kSpecMaxFrames) {
    /* An early start can fail for reasons of its own (the level still
     * setting up): try again once the room is on screen. */
    if (!g_spec.early)
      g_failed[g_spec.target] = 1;
    if (Debug())
      fprintf(stderr, "[rooms_wide] room %02X not captured (%s)\n", g_spec.target,
              lost ? "Conrad did not survive entering it" : "it never finished loading");
    return true;
  }
  return false;
}

/* Speculative frames for up to kSpecBudgetMs, sandwiched between saving and
 * restoring the real machine. */
static void StepSpeculation(void) {
  const uint64_t t0 = SDL_GetPerformanceCounter();
  /* Less while the real screen is blanked: a load shortened by fast_load.c
   * spends its own budget in the same host frame. */
  const double budget_ms = PPU_forcedBlank(g_ppu) ? kSpecBudgetLoadMs : kSpecBudgetMs;
  const uint64_t budget =
      (uint64_t)((double)SDL_GetPerformanceFrequency() * budget_ms / 1000.0);
  const size_t real = RtlRollbackSaveToMemory(g_real_blob, g_blob_cap);
  if (!real || !RtlRollbackLoadFromMemory(g_spec_blob, g_spec.len)) {
    g_spec.active = false;
    if (real)
      RestoreReal(real);
    return;
  }
  bool finished = false;
  int frames = 0;
  for (;;) {
    RunSpeculativeFrame();
    frames++;
    if ((finished = AdvanceSpeculation()))
      break;
    if (frames >= kSpecMaxBatch || SDL_GetPerformanceCounter() - t0 >= budget) {
      g_spec.len = RtlRollbackSaveToMemory(g_spec_blob, g_blob_cap);
      finished = g_spec.len == 0;
      break;
    }
  }
  if (finished)
    g_spec.active = false;
  RestoreReal(real);
  if (Debug()) {
    static double total_ms, worst_ms;
    static int steps, total_frames;
    const double ms = (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 /
                      (double)SDL_GetPerformanceFrequency();
    total_ms += ms;
    total_frames += frames;
    if (ms > worst_ms) worst_ms = ms;
    if (++steps % 60 == 0)
      fprintf(stderr, "[rooms_wide]   speculation: %.2f ms and %.1f frames per real "
              "frame on average, worst %.2f ms\n", total_ms / steps,
              (double)total_frames / steps, worst_ms);
  }
}

/* Before each real guest frame, never after one: the host rasterizes a
 * frame from per-line PPU records collected while it ran, which a snapshot
 * does not hold, so a speculative frame between a real frame and its raster
 * would replace its picture. Here the previous frame is already presented. */
void RoomsWideBeforeRunFrame(void) {
  if (!Enabled())
    return;
  if (g_spec.active) {
    /* The player left the room it was started from: its result still
     * belongs to that level, so let it finish. */
    StepSpeculation();
    if (Debug())
      g_real_frame_start = SDL_GetPerformanceCounter();
    return;
  }
  /* As soon as Conrad is in a room, even while it is still loading or
   * fading in, so its neighbours are ready by the time it is shown. */
  const int room = g_cpu.ram[kRoomVar];
  if (room > 0 && room < 0x40 && g_cpu.ram[kConradRecord + 7] == room &&
      !CutsceneHdActive() &&
      (GameplayOnScreen(g_ppu) || PPU_forcedBlank(g_ppu) || (g_ppu->inidisp & 15) != 15))
    StartSpeculation(LevelKey(), room);
}

void RoomsWideOnReset(void) {
  g_spec.active = false;
  g_stable_frames = 0;
  g_live_matches_cache = false;
}
