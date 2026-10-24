/* HD title screen.
 *
 * The SNES title screen is built from (tools/research/CUTSCENE_RENDERER.md
 * has the method; this is the layout found the same way):
 *   BG2  the Conrad artwork (tilemap $0C00), BG1 the slime and gun glow
 *   BG3  the menu: START / PASSWORD / OPTIONS / DEMO (tilemap $5C00), present
 *        only once the menu is available
 *   OBJ  the FLASHBACK logo and the copyright line
 *
 * While it is on screen this draws, instead of the native frame:
 *   - the pack's 16:9 background, scaled to the canvas
 *   - the pack's logo, fading in once the menu is available
 *   - the menu, decoded from BG3 with the live palette (so selection and
 *     blinking work), at its native position in the pillarboxed 4:3 field
 * The game's brightness applies to all of it, so its fades still work.
 *
 * Pack files (FLASHBACK_HD_PACK, default <exe dir>/hd):
 *   title/background.png   any size, drawn to fill the 16:9 canvas
 *   title/logo.png         RGBA, placed as in the reference mock-up
 * Without them the native title screen is shown.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common_rtl.h"
#include "host_paths.h"
#include "snes/ppu.h"
#include "stb/stb_image.h"
#include "title_hd.h"

enum {
  kBg2Tilemap = 0x0C00,
  kBg3Tilemap = 0x5C00,
  kTitleTilemapHash = 0xCA660375u, /* FNV-1a of the BG2 tilemap, 32x28 words */
  kMenuFirstRow = 15,
  kMenuLastRow = 21,
  kLogoFadeFrames = 30,
  kFieldW = 256,
  kFieldH = 224,
};

/* Logo placement on the canvas: the visible (opaque) part of logo.png spans
 * this fraction of the canvas width, with its top at kLogoTop. */
static const double kLogoLeft = 0.4874, kLogoRight = 0.9402, kLogoTop = 0.2062;

typedef struct Image {
  uint32_t *px;  /* premultiplied ARGB */
  int w, h;
} Image;

static Image g_background_src, g_logo_src, g_background, g_logo;
static int g_logo_x, g_logo_y;
static int g_logo_left, g_logo_right, g_logo_top;  /* opaque bounds in logo.png */
static int g_load_state;  /* 0 not tried, 1 loaded, -1 unavailable */
static int g_menu_since = -1;

/* ---- Pack loading -------------------------------------------------------- */

static bool PackPath(const char *leaf, char *out, size_t cap) {
  const char *pack = getenv("FLASHBACK_HD_PACK");
  if (pack && *pack)
    return snprintf(out, cap, "%s/%s", pack, leaf) < (int)cap;
  char rel[256];
  snprintf(rel, sizeof rel, "hd/%s", leaf);
  return snesrecomp_exe_dir_path(rel, out, cap) != 0;
}

static bool LoadImage(const char *leaf, Image *img) {
  char path[1024];
  int w, h, n;
  if (!PackPath(leaf, path, sizeof path))
    return false;
  uint8_t *rgba = stbi_load(path, &w, &h, &n, 4);
  if (!rgba) {
    fprintf(stderr, "[title_hd] cannot load %s\n", path);
    return false;
  }
  img->px = (uint32_t *)malloc((size_t)w * h * 4);
  if (!img->px) {
    stbi_image_free(rgba);
    return false;
  }
  for (size_t i = 0; i < (size_t)w * h; i++) {
    const uint8_t *p = rgba + i * 4;
    const uint32_t a = p[3];
    img->px[i] = a << 24 | (p[0] * a / 255) << 16 | (p[1] * a / 255) << 8 |
                 (p[2] * a / 255);
  }
  img->w = w;
  img->h = h;
  stbi_image_free(rgba);
  fprintf(stderr, "[title_hd] loaded %s (%dx%d)\n", path, w, h);
  return true;
}

/* The opaque part of the logo, which the placement refers to. */
static bool FindLogoBounds(void) {
  g_logo_left = g_logo_src.w, g_logo_right = 0, g_logo_top = g_logo_src.h;
  for (int y = 0; y < g_logo_src.h; y++)
    for (int x = 0; x < g_logo_src.w; x++)
      if ((g_logo_src.px[(size_t)y * g_logo_src.w + x] >> 24) > 200) {
        if (x < g_logo_left) g_logo_left = x;
        if (x + 1 > g_logo_right) g_logo_right = x + 1;
        if (y < g_logo_top) g_logo_top = y;
      }
  return g_logo_right > g_logo_left;
}

static bool EnsureLoaded(void) {
  if (g_load_state == 0)
    g_load_state = LoadImage("title/background.png", &g_background_src) &&
                   LoadImage("title/logo.png", &g_logo_src) && FindLogoBounds()
                   ? 1 : -1;
  return g_load_state > 0;
}

/* Area-average resample of a premultiplied image into w x h. */
static bool Resample(const Image *src, Image *dst, int w, int h) {
  if (dst->px && dst->w == w && dst->h == h)
    return true;
  free(dst->px);
  dst->px = (uint32_t *)malloc((size_t)w * h * 4);
  if (!dst->px)
    return false;
  dst->w = w;
  dst->h = h;
  for (int y = 0; y < h; y++) {
    int sy0 = y * src->h / h, sy1 = (y + 1) * src->h / h;
    if (sy1 <= sy0) sy1 = sy0 + 1;
    for (int x = 0; x < w; x++) {
      int sx0 = x * src->w / w, sx1 = (x + 1) * src->w / w;
      if (sx1 <= sx0) sx1 = sx0 + 1;
      uint32_t sum[4] = {0, 0, 0, 0};
      for (int sy = sy0; sy < sy1; sy++)
        for (int sx = sx0; sx < sx1; sx++) {
          const uint32_t c = src->px[(size_t)sy * src->w + sx];
          sum[0] += c >> 24;
          sum[1] += (c >> 16) & 255;
          sum[2] += (c >> 8) & 255;
          sum[3] += c & 255;
        }
      const uint32_t n = (uint32_t)((sy1 - sy0) * (sx1 - sx0));
      dst->px[(size_t)y * w + x] = (sum[0] / n) << 24 | (sum[1] / n) << 16 |
                                   (sum[2] / n) << 8 | (sum[3] / n);
    }
  }
  return true;
}

/* Fit both images to the canvas (cached per size). The logo's opaque bounds
 * set its size. */
static bool Layout(int canvas_w, int canvas_h) {
  if (!Resample(&g_background_src, &g_background, canvas_w, canvas_h))
    return false;
  const int left = g_logo_left, top = g_logo_top;
  const double scale = (kLogoRight - kLogoLeft) * canvas_w / (g_logo_right - left);
  const int w = (int)(g_logo_src.w * scale + 0.5), h = (int)(g_logo_src.h * scale + 0.5);
  if (!Resample(&g_logo_src, &g_logo, w, h))
    return false;
  g_logo_x = (int)(kLogoLeft * canvas_w - left * scale + 0.5);
  g_logo_y = (int)(kLogoTop * canvas_h - top * scale + 0.5);
  return true;
}

/* ---- Detection ------------------------------------------------------------ */

static uint32_t TilemapHash(const Ppu *ppu, uint16_t base) {
  uint32_t h = 0x811c9dc5u;
  for (int i = 0; i < 32 * 28; i++) {
    const uint16_t w = ppu->vram[(base + i) & 0x7fff];
    h = (h ^ (w & 255)) * 0x01000193u;
    h = (h ^ (w >> 8)) * 0x01000193u;
  }
  return h;
}

static bool TitleOnScreen(const Ppu *ppu) {
  return ppu && !PPU_forcedBlank(ppu) && PPU_mode(ppu) == 1 &&
         PPU_bgTilemapAdr(ppu, 1) == kBg2Tilemap &&
         PPU_bgTilemapAdr(ppu, 2) == kBg3Tilemap &&
         TilemapHash(ppu, kBg2Tilemap) == kTitleTilemapHash;
}

static bool MenuAvailable(const Ppu *ppu) {
  for (int row = kMenuFirstRow; row <= kMenuLastRow; row++)
    for (int col = 0; col < 32; col++)
      if (ppu->vram[(kBg3Tilemap + row * 32 + col) & 0x7fff] & 0x3ff)
        return true;
  return false;
}

/* ---- Drawing -------------------------------------------------------------- */

/* dst = src (premultiplied) over dst, src scaled by alpha256 and brightness. */
static inline uint32_t Over(uint32_t dst, uint32_t src, uint32_t alpha256,
                            uint32_t bright) {
  const uint32_t a = ((src >> 24) * alpha256) >> 8;
  const uint32_t k = (alpha256 * bright) >> 4;  /* bright is 0..16 */
  uint32_t out = 0xff000000u;
  for (int shift = 0; shift <= 16; shift += 8) {
    const uint32_t s = (((src >> shift) & 255) * k) >> 8;
    const uint32_t d = (dst >> shift) & 255;
    out |= ((s + d * (255 - a) / 255) & 255) << shift;
  }
  return out;
}

static inline uint32_t PpuColour(const Ppu *ppu, uint16_t c) {
  return 0xff000000u | (uint32_t)ppu->brightnessMult[c & 0x1f] << 16 |
         (uint32_t)ppu->brightnessMult[(c >> 5) & 0x1f] << 8 |
         ppu->brightnessMult[(c >> 10) & 0x1f];
}

/* The BG3 menu (2bpp, mode 1), each SNES pixel a block on the canvas at its
 * place in the field drawn field_w x field_h at (field_x, 0). */
static void DrawMenu(const Ppu *ppu, uint8_t *dst, size_t pitch, int field_x,
                     int field_w, int field_h) {
  const int tile_base = PPU_bgTileAdr(ppu, 2);
  const int hs = ppu->hScroll[2] & 0x3ff, vs = ppu->vScroll[2] & 0x3ff;
  for (int row = 0; row < 32; row++)
    for (int col = 0; col < 32; col++) {
      const uint16_t e = ppu->vram[(kBg3Tilemap + row * 32 + col) & 0x7fff];
      const int tile = e & 0x3ff, pal = (e >> 10) & 7;
      if (!tile)
        continue;
      for (int ty = 0; ty < 8; ty++) {
        const uint16_t bits =
            ppu->vram[(tile_base + tile * 8 + ((e & 0x8000) ? 7 - ty : ty)) & 0x7fff];
        /* A BG line shows map row y + vscroll + 1. */
        const int fy = ((row * 8 + ty - vs - 1) & 0x3ff);
        if (fy >= kFieldH)
          continue;
        for (int tx = 0; tx < 8; tx++) {
          const int b = (e & 0x4000) ? tx : 7 - tx;
          const int i = ((bits >> b) & 1) | (((bits >> (8 + b)) & 1) << 1);
          const int fx = (col * 8 + tx - hs) & 0x3ff;
          if (!i || fx >= kFieldW)
            continue;
          const uint32_t c = PpuColour(ppu, ppu->cgram[pal * 4 + i]);
          const int x0 = field_x + fx * field_w / kFieldW;
          const int x1 = field_x + (fx + 1) * field_w / kFieldW;
          const int y0 = fy * field_h / kFieldH, y1 = (fy + 1) * field_h / kFieldH;
          for (int y = y0; y < y1; y++) {
            uint32_t *out = (uint32_t *)(dst + (size_t)y * pitch);
            for (int x = x0; x < x1; x++)
              out[x] = c;
          }
        }
      }
    }
}

void TitleHdOnReset(void) {
  g_menu_since = -1;
}

bool TitleHdDraw(uint8_t *dst, size_t pitch, int canvas_w, int canvas_h,
                 int field_x, int field_w) {
  const Ppu *ppu = g_ppu;
  if (!TitleOnScreen(ppu)) {
    g_menu_since = -1;
    return false;
  }
  if (!EnsureLoaded() || !Layout(canvas_w, canvas_h))
    return false;

  const uint32_t bright = (uint32_t)(ppu->inidisp & 15) + ((ppu->inidisp & 15) ? 1 : 0);
  for (int y = 0; y < canvas_h; y++) {
    uint32_t *row = (uint32_t *)(dst + (size_t)y * pitch);
    const uint32_t *src = g_background.px + (size_t)y * canvas_w;
    for (int x = 0; x < canvas_w; x++)
      row[x] = Over(0xff000000u, src[x], 256, bright);
  }

  if (MenuAvailable(ppu)) {
    if (g_menu_since < 0)
      g_menu_since = snes_frame_counter;
    int t = snes_frame_counter - g_menu_since;
    const uint32_t alpha256 = t >= kLogoFadeFrames ? 256 : (uint32_t)(t * 256 / kLogoFadeFrames);
    for (int y = 0; y < g_logo.h; y++) {
      const int cy = g_logo_y + y;
      if (cy < 0 || cy >= canvas_h)
        continue;
      uint32_t *row = (uint32_t *)(dst + (size_t)cy * pitch);
      const uint32_t *src = g_logo.px + (size_t)y * g_logo.w;
      for (int x = 0; x < g_logo.w; x++) {
        const int cx = g_logo_x + x;
        if (cx >= 0 && cx < canvas_w && src[x] >> 24)
          row[cx] = Over(row[cx], src[x], alpha256, bright);
      }
    }
    DrawMenu(ppu, dst, pitch, field_x, field_w, canvas_h);
  } else {
    g_menu_since = -1;
  }
  return true;
}
