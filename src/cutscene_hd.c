/* HD cutscenes.
 *
 * Flashback draws its cinematics as flat-coloured polygons and ellipses,
 * rasterized by the CPU into one 4bpp bitmap at $7E:2000 and DMAed whole to
 * VRAM after each batch (tools/research/CUTSCENE_RENDERER.md):
 *   - to $3000 or $5000: BG2, the static scene background (double buffered)
 *   - to $1000: BG1, the animated foreground, redrawn every keyframe
 *
 * Capture is passive: an interpreter PC hook watches two instructions of the
 * shape decoder and reads the shape from guest memory without touching CPU
 * state, so the game runs exactly as in the plain build. (Wrapping the
 * routines with hle_func changed cycle timing, and the cinematic engine's
 * keyframe pacing with it.) This relies on bank $C1 being interpreted, as it
 * is while recomp/bankc1.cfg declares nothing for it:
 *   $C10127  JSL $C101C2   polygon: vertex list at $02A2, count in dp $14
 *   $C10073  JSR $C14F0A   ellipse: centre and radii from the shape record
 *                          at [$20] plus the origin in dp $18/$1C
 *   $C1625A  STA $420B             a WRAM DMA zeroes the bitmap: a new batch
 * The VRAM DMA hook files everything drawn since the last clear under the
 * layer it went to (the engine sometimes draws over an uploaded bitmap and
 * uploads it again, as the jungle behind the level 1 wake-up does).
 *
 * While a cutscene is on screen, CutsceneHdDraw (called by present.c)
 * redraws both layers from the recorded shapes at the size it is given,
 * with the live palette and brightness. Each native pixel is checked against
 * the PPU's own output first: wherever the redraw disagrees beyond edge
 * rounding (sprites, a primitive not captured, a layer we never saw drawn)
 * the native pixel is shown instead, so the enhancement can never hide guest
 * output.
 *
 * Environment:
 *   FLASHBACK_POLY_LOG=f   also log shapes and VRAM DMAs to file f:
 *       <frame> <colour $1E52> <vertex count> x0 y0 x1 y1 ...
 *       <frame> ell <colour $1E52> <centre x> <centre y> <radius x> <radius y>
 *       <frame> dma <source pc24> <vram word address> <bytes>
 */

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common_rtl.h"
#include "cpu_state.h"
#include "cutscene_hd.h"
#include "snes/dma.h"
#include "snes/interp_bridge.h"
#include "snes/ppu.h"

enum {
  kPolygonSitePc = 0xC10127,
  kEllipseSitePc = 0xC10073,
  kClearSitePc = 0xC1625A,  /* STA $420B: DMA zeroes the bitmap */
  kShapePointerDp = 0x20,
  kOriginXDp = 0x18,
  kOriginYDp = 0x1C,
  kVertexCountDp = 0x14,
  kVertexList = 0x02A2,
  kColour = 0x1E52,
  kMaxVertices = 64,
  kMaxShapes = 2048,

  kBitmapSource = 0x7E2000,
  kForegroundVram = 0x1000,
  kBackgroundVramA = 0x3000,
  kBackgroundVramB = 0x5000,
  kBg1TilemapVram = 0x0000,
  kBg2TilemapVram = 0x0400,

  kViewW = 240,
  kViewH = 128,
  kFieldW = 256,
  kFieldH = 224,
};

enum { kShapePolygon, kShapeEllipse };

typedef struct Shape {
  uint8 kind;
  uint8 colour;
  uint8 count;                 /* polygon vertices */
  int16 xy[kMaxVertices * 2];  /* polygon: x0 y0 x1 y1 ...;
                                  ellipse: centre x, centre y, radius x, y */
} Shape;

typedef struct ShapeBatch {
  Shape shapes[kMaxShapes];
  int count;
  bool overflow;
  unsigned generation;  /* bumped whenever the contents change */
} ShapeBatch;

/* The batch being drawn, and the last batch uploaded to each layer. */
static ShapeBatch g_pending, g_foreground, g_background[2];
static bool g_foreground_valid, g_background_valid[2];
static unsigned g_generation;
static FILE *g_log;
static int g_last_keyframe;  /* frame of the last foreground upload */
static uint8 *g_rom_copy;
static size_t g_rom_copy_size;

/* ---- Guest memory, read without side effects ---------------------------- */

static uint8 Peek(uint8 bank, uint16 addr) {
  if (bank == 0x7E || bank == 0x7F)
    return g_cpu.ram[((size_t)(bank & 1) << 16) | addr];
  const uint8 b = bank & 0x7F;
  if (b < 0x40 && addr < 0x2000)
    return g_cpu.ram[addr];
  if (b >= 0x40 || addr >= 0x8000) {  /* HiROM */
    const size_t off = ((size_t)(bank & 0x3F) << 16) | addr;
    return off < g_rom_copy_size ? g_rom_copy[off] : 0;
  }
  return 0;  /* I/O and unmapped: never read by the shape data */
}

static uint16 Peek16(uint8 bank, uint16 addr) {
  return (uint16)(Peek(bank, addr) | Peek(bank, (uint16)(addr + 1)) << 8);
}

static uint16 PeekDp16(uint8 dp) {
  return Peek16(0x00, (uint16)(g_cpu.D + dp));
}

/* ---- Capture ------------------------------------------------------------ */

static Shape *NewShape(void) {
  if (g_pending.count >= kMaxShapes) {
    g_pending.overflow = true;
    return NULL;
  }
  return &g_pending.shapes[g_pending.count++];
}

static void RecordPolygon(void) {
  const uint16 count = PeekDp16(kVertexCountDp);
  const uint16 colour = Peek16(0x00, kColour);
  if (count > kMaxVertices) {
    if (g_log)
      fprintf(g_log, "%d %u %u !overflow\n", snes_frame_counter, colour, count);
    g_pending.overflow = true;
    return;
  }
  Shape *shape = NewShape();
  if (g_log)
    fprintf(g_log, "%d %u %u", snes_frame_counter, colour, count);
  for (uint16 i = 0; i < count; i++) {
    const int16 x = (int16)Peek16(0x00, (uint16)(kVertexList + i * 4));
    const int16 y = (int16)Peek16(0x00, (uint16)(kVertexList + i * 4 + 2));
    if (shape) {
      shape->xy[i * 2] = x;
      shape->xy[i * 2 + 1] = y;
    }
    if (g_log)
      fprintf(g_log, " %d %d", x, y);
  }
  if (g_log)
    fputc('\n', g_log);
  if (shape) {
    shape->kind = kShapePolygon;
    shape->colour = (uint8)((colour >> 2) & 15);
    shape->count = (uint8)count;
  }
}

/* The decoder pushes big-endian words from the record: x + origin x,
 * y + origin y, radius x, radius y. */
static void RecordEllipse(void) {
  const uint16 ptr = PeekDp16(kShapePointerDp);
  const uint8 bank = Peek(0x00, (uint16)(g_cpu.D + kShapePointerDp + 2));
  int16 word[4];
  for (int i = 0; i < 4; i++) {
    const uint16 a = (uint16)(ptr + i * 2);
    word[i] = (int16)(Peek(bank, a) << 8 | Peek(bank, (uint16)(a + 1)));
  }
  const int16 cx = (int16)(word[0] + PeekDp16(kOriginXDp));
  const int16 cy = (int16)(word[1] + PeekDp16(kOriginYDp));
  const uint16 colour = Peek16(0x00, kColour);
  if (g_log)
    fprintf(g_log, "%d ell %u %d %d %d %d\n", snes_frame_counter, colour, cx, cy,
            word[2], word[3]);
  Shape *shape = NewShape();
  if (!shape)
    return;
  shape->kind = kShapeEllipse;
  shape->colour = (uint8)((colour >> 2) & 15);
  shape->count = 0;
  shape->xy[0] = cx;
  shape->xy[1] = cy;
  shape->xy[2] = word[2];
  shape->xy[3] = word[3];
}

static void OnInterpretedOpcode(uint32_t pc24, int m_flag, int x_flag) {
  (void)m_flag;
  (void)x_flag;
  if (pc24 == kPolygonSitePc) {
    RecordPolygon();
  } else if (pc24 == kEllipseSitePc) {
    RecordEllipse();
  } else if (pc24 == kClearSitePc) {
    g_pending.count = 0;
    g_pending.overflow = false;
  }
}

static void TakeBatch(ShapeBatch *dst, bool *valid) {
  memcpy(dst->shapes, g_pending.shapes, sizeof(Shape) * (size_t)g_pending.count);
  dst->count = g_pending.count;
  dst->overflow = g_pending.overflow;
  dst->generation = ++g_generation;
  *valid = !g_pending.overflow;
}

static void OnVramDma(uint8_t bank, uint16_t addr, uint16_t vram_word,
                      uint16_t size) {
  const uint32 source = ((uint32)bank << 16) | addr;
  if (g_log)
    fprintf(g_log, "%d dma %06X %04X %u\n", snes_frame_counter, source,
            vram_word, size ? size : 0x10000u);
  if (source != kBitmapSource)
    return;
  if (vram_word == kForegroundVram) {
    TakeBatch(&g_foreground, &g_foreground_valid);
    g_last_keyframe = snes_frame_counter;
  }
  else if (vram_word == kBackgroundVramA)
    TakeBatch(&g_background[0], &g_background_valid[0]);
  else if (vram_word == kBackgroundVramB)
    TakeBatch(&g_background[1], &g_background_valid[1]);
  /* The foreground is redrawn from scratch for every keyframe. A
   * background is not: the engine may draw more over an uploaded one and
   * upload it again, so its batch runs on until the bitmap is cleared. */
  if (vram_word == kForegroundVram) {
    g_pending.count = 0;
    g_pending.overflow = false;
  }
}

void CutsceneHdOnRomLoaded(const uint8_t *rom, size_t size) {
  if (size % 1024 == 512) {  /* copier header */
    rom += 512;
    size -= 512;
  }
  free(g_rom_copy);
  g_rom_copy = (uint8 *)malloc(size);
  g_rom_copy_size = g_rom_copy ? size : 0;
  if (g_rom_copy)
    memcpy(g_rom_copy, rom, size);
  const char *path = getenv("FLASHBACK_POLY_LOG");
  if (path && *path && !g_log)
    g_log = fopen(path, "w");
  dma_set_vram_notify_hook(OnVramDma);
  g_interp_bridge_pc_hook = OnInterpretedOpcode;
  g_interp_fast_bank = 0xC1;
}

bool CutsceneHdLayersOnScreen(void) {
  const Ppu *ppu = g_ppu;
  return ppu && PPU_mode(ppu) == 1 && PPU_bgTilemapAdr(ppu, 0) == kBg1TilemapVram &&
         PPU_bgTilemapAdr(ppu, 1) == kBg2TilemapVram &&
         PPU_bgTileAdr(ppu, 0) == kForegroundVram;
}

/* Cutscene pace. The SNES engine draws each keyframe on the CPU and shows
 * it when done, so cinematics run as fast as the rasterizer: a keyframe
 * every 6 to 11 frames, against the PC version's 100 ms (6 frames at
 * 60 Hz). Once a keyframe has been drawing for that long, the renderer's
 * bank ($C1) is made 16 times cheaper in guest time, so it finishes at once;
 * quicker keyframes are left alone. Frames still end on the beam, so sound
 * and music keep real time.
 *   FLASHBACK_CUTSCENE_FRAMES=N   frames per keyframe at most (6 is the PC
 *                                 pace); default 0, the SNES pace: the
 *                                 engine never waits for its music, so a
 *                                 faster picture runs ahead of the sound */
void CutsceneHdBeforeRunFrame(void) {
  static int target = -1;
  if (target < 0) {
    const char *v = getenv("FLASHBACK_CUTSCENE_FRAMES");
    target = v && *v ? atoi(v) : 0;
  }
  /* Only with the cinematic layers on screen: gameplay calls into bank $C1
   * too, and must keep its own timing. */
  const bool cinematic = CutsceneHdLayersOnScreen();
  g_interp_fast_all = false;
  g_interp_fast_divisor = cinematic && target > 0 &&
                                  snes_frame_counter - g_last_keyframe >= target
                              ? 16 : 0;
}

/* The timeline jumped (reset, state load, rewind): what we recorded no longer
 * describes VRAM, so present natively until the layers are drawn again. */
void CutsceneHdOnReset(void) {
  g_pending.count = 0;
  g_pending.overflow = false;
  g_foreground_valid = false;
  g_background_valid[0] = g_background_valid[1] = false;
}


/* ---- Rasterizer ----------------------------------------------------------
 *
 * Shapes are drawn into colour-index buffers (0 = transparent) covering the
 * 240x128 viewport at any size, so x and y scale independently. The SNES
 * fills one span per scanline, inclusive of both edge pixels, so a shape is
 * the filled polygon plus its edges stroked one SNES pixel wide; that keeps
 * slivers and two-vertex lines at every scale. Vertex (x, y) is the centre of
 * SNES pixel (x, y). */

typedef struct IndexBuffer {
  uint8 *pixels;
  int w, h;
  unsigned generation;
} IndexBuffer;

/* Fill one span per scanline from the leftmost to the rightmost edge crossing
 * of pts (n points, buffer pixel units), sampling pixel centres. */
static void FillSpans(IndexBuffer *buf, const float *pts, int n, uint8 colour) {
  float min_y = pts[1], max_y = pts[1];
  for (int i = 1; i < n; i++) {
    if (pts[i * 2 + 1] < min_y) min_y = pts[i * 2 + 1];
    if (pts[i * 2 + 1] > max_y) max_y = pts[i * 2 + 1];
  }
  int y0 = (int)(min_y - 0.5f), y1 = (int)(max_y + 0.5f);
  if (y0 < 0) y0 = 0;
  if (y1 >= buf->h) y1 = buf->h - 1;
  for (int y = y0; y <= y1; y++) {
    const float cy = (float)y + 0.5f;
    float left = 1e9f, right = -1e9f;
    for (int i = 0; i < n; i++) {
      const float *a = &pts[i * 2], *b = &pts[((i + 1) % n) * 2];
      if ((a[1] <= cy && b[1] >= cy) || (b[1] <= cy && a[1] >= cy)) {
        float lo, hi;
        if (a[1] == b[1]) {
          lo = a[0] < b[0] ? a[0] : b[0];
          hi = a[0] > b[0] ? a[0] : b[0];
        } else {
          lo = hi = a[0] + (cy - a[1]) * (b[0] - a[0]) / (b[1] - a[1]);
        }
        if (lo < left) left = lo;
        if (hi > right) right = hi;
      }
    }
    if (left > right)
      continue;
    int x0 = (int)(left + 0.5f), x1 = (int)(right - 0.5f);
    if (x0 < 0) x0 = 0;
    if (x1 >= buf->w) x1 = buf->w - 1;
    if (x0 <= x1)
      memset(buf->pixels + (size_t)y * buf->w + x0, colour, (size_t)(x1 - x0 + 1));
  }
}

/* An edge stroked one SNES pixel wide: the convex hull of a pixel-sized
 * rectangle swept from a to b (its 8 corners, in angular order). */
static void StrokeEdge(IndexBuffer *buf, float ax, float ay, float bx, float by,
                       float hx, float hy, uint8 colour) {
  float corners[8][2] = {
      {ax - hx, ay - hy}, {ax + hx, ay - hy}, {ax + hx, ay + hy}, {ax - hx, ay + hy},
      {bx - hx, by - hy}, {bx + hx, by - hy}, {bx + hx, by + hy}, {bx - hx, by + hy},
  };
  const float cx = (ax + bx) * 0.5f, cy = (ay + by) * 0.5f;
  float angle[8];
  for (int i = 0; i < 8; i++)
    angle[i] = atan2f(corners[i][1] - cy, corners[i][0] - cx);
  for (int i = 1; i < 8; i++)
    for (int j = i; j > 0 && angle[j] < angle[j - 1]; j--) {
      float t = angle[j]; angle[j] = angle[j - 1]; angle[j - 1] = t;
      float u0 = corners[j][0], u1 = corners[j][1];
      corners[j][0] = corners[j - 1][0]; corners[j][1] = corners[j - 1][1];
      corners[j - 1][0] = u0; corners[j - 1][1] = u1;
    }
  FillSpans(buf, &corners[0][0], 8, colour);
}

/* A filled ellipse. The SNES covers whole pixels out to the radius on both
 * sides of the centre pixel, so the continuous extent is radius + 1/2. */
static void FillEllipse(IndexBuffer *buf, const Shape *shape) {
  const float sx = (float)buf->w / kViewW, sy = (float)buf->h / kViewH;
  const float cx = ((float)shape->xy[0] + 0.5f) * sx;
  const float cy = ((float)shape->xy[1] + 0.5f) * sy;
  const float rx = ((float)shape->xy[2] + 0.5f) * sx;
  const float ry = ((float)shape->xy[3] + 0.5f) * sy;
  if (rx <= 0 || ry <= 0)
    return;
  int y0 = (int)(cy - ry), y1 = (int)(cy + ry);
  if (y0 < 0) y0 = 0;
  if (y1 >= buf->h) y1 = buf->h - 1;
  for (int y = y0; y <= y1; y++) {
    const float t = ((float)y + 0.5f - cy) / ry;
    if (t < -1.0f || t > 1.0f)
      continue;
    const float half = rx * sqrtf(1.0f - t * t);
    int x0 = (int)(cx - half + 0.5f), x1 = (int)(cx + half - 0.5f);
    if (x0 < 0) x0 = 0;
    if (x1 >= buf->w) x1 = buf->w - 1;
    if (x0 <= x1)
      memset(buf->pixels + (size_t)y * buf->w + x0, shape->colour, (size_t)(x1 - x0 + 1));
  }
}

static void DrawShape(IndexBuffer *buf, const Shape *shape) {
  if (shape->kind == kShapeEllipse) {
    FillEllipse(buf, shape);
    return;
  }
  const float sx = (float)buf->w / kViewW, sy = (float)buf->h / kViewH;
  const float hx = sx * 0.5f, hy = sy * 0.5f;
  float pts[kMaxVertices * 2];
  for (int i = 0; i < shape->count; i++) {
    pts[i * 2] = ((float)shape->xy[i * 2] + 0.5f) * sx;
    pts[i * 2 + 1] = ((float)shape->xy[i * 2 + 1] + 0.5f) * sy;
  }
  if (shape->count >= 3)
    FillSpans(buf, pts, shape->count, shape->colour);
  const int edges = shape->count == 2 ? 1 : shape->count;
  for (int i = 0; i < edges; i++) {
    int j = (i + 1) % shape->count;
    StrokeEdge(buf, pts[i * 2], pts[i * 2 + 1], pts[j * 2], pts[j * 2 + 1],
               hx, hy, shape->colour);
  }
}

static bool Rasterize(IndexBuffer *buf, const ShapeBatch *batch, int w, int h) {
  if (buf->w != w || buf->h != h || !buf->pixels) {
    free(buf->pixels);
    buf->pixels = (uint8 *)malloc((size_t)w * h);
    if (!buf->pixels)
      return false;
    buf->w = w;
    buf->h = h;
    buf->generation = 0;
  }
  if (buf->generation == batch->generation)
    return true;
  memset(buf->pixels, 0, (size_t)w * h);
  for (int i = 0; i < batch->count; i++)
    DrawShape(buf, &batch->shapes[i]);
  buf->generation = batch->generation;
  return true;
}

/* ---- Presentation ------------------------------------------------------- */

/* An SNES colour at master brightness 0..15, as the PPU outputs it. */
static inline uint32 Colour(uint16 c, int brightness) {
  const uint32 r = c & 0x1f, g = (c >> 5) & 0x1f, b = (c >> 10) & 0x1f;
  return (((r << 3) | (r >> 2)) * brightness / 15) << 16 |
         (((g << 3) | (g >> 2)) * brightness / 15) << 8 |
         ((b << 3) | (b >> 2)) * brightness / 15;
}

/* Layer palettes come from the first tilemap entry of each bitmap layer:
 * 16 colours of BG1, 16 of BG2, then the backdrop. */
static void Palette(const Ppu *ppu, int brightness, uint32 out[33]) {
  const int fg = (ppu->vram[kBg1TilemapVram & 0x7fff] >> 10) & 7;
  const int bg = (ppu->vram[kBg2TilemapVram & 0x7fff] >> 10) & 7;
  for (int i = 0; i < 16; i++) {
    out[i] = Colour(ppu->cgram[fg * 16 + i], brightness);
    out[16 + i] = Colour(ppu->cgram[bg * 16 + i], brightness);
  }
  out[32] = Colour(ppu->cgram[0], brightness);
}

/* Which BG2 buffer is on screen while a cutscene is shown, or -1.
 *
 * Forced blank is deliberately not tested: these are the registers as the
 * frame ends, and the cinematic engine sets forced blank after the visible
 * lines of every other frame to upload, so testing it made the picture flip
 * between HD and native each frame. A frame that really was blanked shows
 * black in the field, and the agreement test in CutsceneHdDraw passes that
 * through. */
static int CutsceneBackgroundSlot(const Ppu *ppu) {
  if (!ppu || PPU_mode(ppu) != 1)
    return -1;
  if ((ppu->screenEnabled[0] & 3) != 3)
    return -1;
  if (PPU_bgTilemapAdr(ppu, 0) != kBg1TilemapVram ||
      PPU_bgTilemapAdr(ppu, 1) != kBg2TilemapVram ||
      PPU_bgTileAdr(ppu, 0) != kForegroundVram)
    return -1;
  const int bg2 = PPU_bgTileAdr(ppu, 1);
  const int slot = bg2 == kBackgroundVramA ? 0 : bg2 == kBackgroundVramB ? 1 : -1;
  if (slot < 0 || !g_foreground_valid || !g_background_valid[slot])
    return -1;
  return slot;
}

bool CutsceneHdActive(void) {
  return CutsceneBackgroundSlot(g_ppu) >= 0;
}

static IndexBuffer g_fg_native, g_bg_native, g_fg_hd, g_bg_hd;

bool CutsceneHdDraw(uint8_t *dst, size_t pitch, int x0, int y0, int w, int h,
                    const uint8_t *field) {
  const Ppu *ppu = g_ppu;
  const int slot = CutsceneBackgroundSlot(ppu);
  if (slot < 0 || w < kViewW || h < kViewH)
    return false;
  const ShapeBatch *bg = &g_background[slot];
  if (!Rasterize(&g_fg_native, &g_foreground, kViewW, kViewH) ||
      !Rasterize(&g_bg_native, bg, kViewW, kViewH) ||
      !Rasterize(&g_fg_hd, &g_foreground, w, h) || !Rasterize(&g_bg_hd, bg, w, h))
    return false;

  /* Viewport origin in the field. A BG line shows scroll row y + vscroll + 1. */
  const int ox = (-(int)ppu->hScroll[0]) & 0x3ff;
  const int oy = ((-(int)ppu->vScroll[0]) & 0x3ff) - 1;
  const uint32 *src = (const uint32 *)field;

  /* The colours the frame was shown with. The registers as the frame ends
   * are no guide: the cinematic engine writes the next frame's brightness
   * and palettes after the visible lines, so colours read from them were
   * often a step off for whole scenes and every pixel fell back to native.
   * Instead each colour is taken from the field: for each layer index, the
   * native colour seen most often inside its shapes (pixels whose
   * neighbours share the index, so edge rounding does not count). Indices
   * not on screen keep the palette's colour. */
  /* The fallback for those: the palette at the master brightness whose
   * colours match the field best (INIDISP as the frame ends is a frame
   * ahead during fades, like the palettes). */
  int brightness = PPU_brightness(ppu), best_hits = -1;
  for (int level = 0; level <= 15; level++) {
    uint32 trial[33];
    Palette(ppu, level, trial);
    int hits = 0;
    for (int y = 1; y < kViewH; y += 4)
      for (int x = 1; x < kViewW; x += 4) {
        const int sx = x + ox, sy = y + oy;
        if (sx < 0 || sx >= kFieldW || sy < 0 || sy >= kFieldH)
          continue;
        const uint8 f = g_fg_native.pixels[y * kViewW + x];
        const uint8 b = g_bg_native.pixels[y * kViewW + x];
        hits += trial[f ? f : b ? 16 + b : 32] == (src[sy * kFieldW + sx] & 0xffffff);
      }
    if (hits > best_hits || (hits == best_hits && level == PPU_brightness(ppu))) {
      best_hits = hits;
      brightness = level;
    }
  }
  uint32 pal[33];
  Palette(ppu, brightness, pal);
  enum { kCandidates = 4 };
  static uint32 cand[33][kCandidates];
  static int votes[33][kCandidates];
  memset(votes, 0, sizeof votes);
  for (int y = 1; y < kViewH - 1; y++)
    for (int x = 1; x < kViewW - 1; x++) {
      const int sx = x + ox, sy = y + oy;
      if (sx < 0 || sx >= kFieldW || sy < 0 || sy >= kFieldH)
        continue;
      const uint8 f = g_fg_native.pixels[y * kViewW + x];
      const uint8 b = g_bg_native.pixels[y * kViewW + x];
      const int key = f ? f : b ? 16 + b : 32;
      bool interior = true;
      for (int ny = y - 1; ny <= y + 1 && interior; ny++)
        for (int nx = x - 1; nx <= x + 1; nx++) {
          const uint8 nf = g_fg_native.pixels[ny * kViewW + nx];
          const uint8 nb = g_bg_native.pixels[ny * kViewW + nx];
          if ((nf ? nf : nb ? 16 + nb : 32) != key) {
            interior = false;
            break;
          }
        }
      if (!interior)
        continue;
      const uint32 c = src[sy * kFieldW + sx] & 0xffffff;
      int slot = -1, weakest = 0;
      for (int i = 0; i < kCandidates; i++) {
        if (votes[key][i] && cand[key][i] == c) {
          slot = i;
          break;
        }
        if (votes[key][i] < votes[key][weakest])
          weakest = i;
      }
      if (slot < 0) {
        slot = weakest;
        cand[key][slot] = c;
        votes[key][slot] = 0;
      }
      votes[key][slot]++;
    }
  for (int k = 0; k < 33; k++) {
    int best = 0;
    for (int i = 1; i < kCandidates; i++)
      if (votes[k][i] > votes[k][best])
        best = i;
    if (votes[k][best] >= 32)
      pal[k] = cand[k][best];
  }
  const uint32 *fg_colour = pal, *bg_colour = pal + 16;
  const uint32 backdrop = pal[32];

  /* The redraw at 1x, then which native pixels it explains. A pixel agrees if
   * the PPU shows the predicted colour there or at a neighbour (edge
   * rounding). One that disagrees on two frames running is shown natively,
   * nearest-neighbour scaled. A single frame is not enough: around keyframe
   * changes and the engine's re-uploads the PPU is often a frame out of step
   * with the recorded shapes, and falling back for that frame alone made the
   * shape outlines flicker between HD and native. */
  static uint32 predicted[kViewH][kViewW], native[kViewH][kViewW];
  static bool agree[kViewH][kViewW], disagreed_before[kViewH][kViewW];
  static int mask_frame = -1;
  const bool new_frame = mask_frame != snes_frame_counter;
  mask_frame = snes_frame_counter;
  for (int y = 0; y < kViewH; y++)
    for (int x = 0; x < kViewW; x++) {
      uint8 f = g_fg_native.pixels[y * kViewW + x];
      uint8 b = g_bg_native.pixels[y * kViewW + x];
      const int sx = x + ox, sy = y + oy;
      native[y][x] = sx >= 0 && sx < kFieldW && sy >= 0 && sy < kFieldH
                         ? src[sy * kFieldW + sx] & 0xffffff : backdrop;
      /* Where no shape is drawn, the field shows what the engine put there
       * without shapes: part of a background can be a painted bitmap it
       * unpacks before drawing over it (the jungle behind the level 1
       * wake-up). That is shown as it is, under the HD shapes. */
      predicted[y][x] = f ? fg_colour[f] : b ? bg_colour[b] : native[y][x];
    }
  for (int y = 0; y < kViewH; y++)
    for (int x = 0; x < kViewW; x++) {
      bool ok = false;
      for (int ny = y - 1; ny <= y + 1 && !ok; ny++)
        for (int nx = x - 1; nx <= x + 1; nx++)
          if (ny >= 0 && ny < kViewH && nx >= 0 && nx < kViewW &&
              predicted[ny][nx] == native[y][x]) {
            ok = true;
            break;
          }
      if (new_frame) {
        agree[y][x] = ok || !disagreed_before[y][x];
        disagreed_before[y][x] = !ok;
      }
    }

  for (int y = 0; y < h; y++) {
    uint32 *row = (uint32 *)(dst + (size_t)(y0 + y) * pitch) + x0;
    const int vy = y * kViewH / h;
    const uint8 *fg = g_fg_hd.pixels + (size_t)y * w;
    const uint8 *bgp = g_bg_hd.pixels + (size_t)y * w;
    for (int x = 0; x < w; x++) {
      const int vx = x * kViewW / w;
      uint32 c;
      if (agree[vy][vx])
        c = fg[x] ? fg_colour[fg[x]]
            : bgp[x] ? bg_colour[bgp[x]]
            /* painted background, where no shape covers the pixel */
            : !g_fg_native.pixels[vy * kViewW + vx] && !g_bg_native.pixels[vy * kViewW + vx]
                ? native[vy][vx] : backdrop;
      else
        c = native[vy][vx];
      row[x] = 0xff000000u | c;
    }
  }
  return true;
}
