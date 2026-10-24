/* Widescreen presentation.
 *
 * Flashback HD always presents a 16:9 frame: a canvas of 398x224 logical
 * pixels (square) times the presentation scale. What fills it:
 *   - cutscenes: the 240x128 cinematic viewport, redrawn in HD and stretched
 *     to the full canvas width at the configured SNES pixel aspect
 *     (cutscene_hd.c), letterboxed top and bottom
 *   - the title screen: the pack's 16:9 art, logo and the live menu
 *     (title_hd.c)
 *   - everything else: the native 256x224 field at the configured pixel
 *     aspect, centred; during gameplay the side areas show the neighbouring
 *     rooms (rooms_wide.c); elsewhere they stay black
 *
 * Environment:
 *   FLASHBACK_HD_SCALE=N   presentation scale, 2 to 8 (1 counts as 2); default 4
 */

#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "cutscene_hd.h"
#include "display_aspect.h"
#include "present.h"
#include "rooms_wide.h"
#include "title_hd.h"

enum {
  kCanvasW = 398,  /* 224 * 16 / 9, even */
  kCanvasH = 224,
  kFieldW = 256,
  kFieldH = 224,
  kMaxScale = 8,
  kCutsceneViewW = 240,
  kCutsceneViewH = 128,
};

static int g_present_scale = -1;

int FlashbackPresentScale(void) {
  if (g_present_scale < 0) {
    const char *v = getenv("FLASHBACK_HD_SCALE");
    int scale = v && *v ? atoi(v) : 4;
    g_present_scale = scale < 1 ? 1 : (scale > kMaxScale ? kMaxScale : scale);
  }
  /* The host treats a scale below 2 as "native": still ask for 2 so the
   * frame keeps its 16:9 canvas through draw_frame. */
  return g_present_scale < 2 ? 2 : g_present_scale;
}

void FlashbackPrepareFrame(int drawable_w, int drawable_h, int *frame_w,
                           int *frame_h) {
  (void)drawable_w;
  (void)drawable_h;
  *frame_w = kCanvasW;
  *frame_h = kCanvasH;
}

/* A 16:9 window on the host's 240-line base height. */
int FlashbackWindowBaseWidth(int frame_w) {
  (void)frame_w;
  return 427;
}

void FlashbackComputeViewport(int frame_w, int frame_h, int drawable_w,
                              int drawable_h, SnesDisplayViewport *viewport) {
  (void)frame_w;
  (void)frame_h;
  if (g_config.ignore_aspect_ratio)
    return;
  *viewport = SnesDisplayAspect_FitViewport(16.0 / 9.0, drawable_w, drawable_h);
}

/* Width on the canvas of `native` SNES pixels at the configured aspect. */
static int DisplayWidth(int native, int canvas_scale) {
  int num, den;
  SnesDisplayAspect_GetPixelAspect(SnesDisplayAspect_Clamp(g_config.display_aspect),
                                   &num, &den);
  return (native * num * canvas_scale + den / 2) / den;
}

/* Native field columns each 16:9 side area shows, rounded up. */
int FlashbackSideColumns(void) {
  const int field_w = DisplayWidth(kFieldW, 64);
  const int side = (kCanvasW * 64 - field_w) / 2;
  return (side * kFieldW + field_w - 1) / field_w;
}

static void Clear(uint8_t *dst, size_t pitch, int w, int h) {
  for (int y = 0; y < h; y++) {
    uint32_t *row = (uint32_t *)(dst + (size_t)y * pitch);
    for (int x = 0; x < w; x++)
      row[x] = 0xff000000u;
  }
}

/* The native field at the configured pixel aspect, from canvas column x0
 * (which may lie off the canvas on either side). */
static void DrawField(uint8_t *dst, size_t pitch, const uint8_t *field,
                      int frame_w, int scale, int x0) {
  const int w = DisplayWidth(kFieldW, scale);
  const int h = kFieldH * scale;
  const int from = x0 < 0 ? -x0 : 0;
  const int to = x0 + w > frame_w ? frame_w - x0 : w;
  const uint32_t *src = (const uint32_t *)field;
  for (int y = 0; y < h; y++) {
    uint32_t *row = (uint32_t *)(dst + (size_t)y * pitch) + x0;
    const uint32_t *line = src + (size_t)(y / scale) * kFieldW;
    for (int x = from; x < to; x++)
      row[x] = line[x * kFieldW / w];
  }
}

int FlashbackDrawFrame(uint8_t *dst, size_t pitch, const uint8_t *field,
                       int frame_w, int frame_h, double alpha) {
  (void)alpha;
  const int scale = frame_h / kCanvasH;
  if (scale < 1 || frame_h != kCanvasH * scale || frame_w != kCanvasW * scale)
    return 0;
  Clear(dst, pitch, frame_w, frame_h);

  const int field_w = DisplayWidth(kFieldW, scale);
  if (TitleHdDraw(dst, pitch, frame_w, frame_h, (frame_w - field_w) / 2, field_w))
    return 1;
  if (CutsceneHdActive()) {
    const int w = frame_w;
    const int h = kCutsceneViewH * w * kFieldW / DisplayWidth(kFieldW, 1) /
                  kCutsceneViewW;
    if (h <= frame_h &&
        CutsceneHdDraw(dst, pitch, 0, (frame_h - h) / 2, w, h, field))
      return 1;
  }
  const int field_x = (frame_w - field_w) / 2;
  RoomsWideDraw(dst, pitch, frame_w, frame_h, field_x, field_w);
  DrawField(dst, pitch, field, frame_w, scale, field_x);
  RoomsWideDrawLabels(dst, pitch, frame_w, frame_h, field_x, field_w);
  return 1;
}
