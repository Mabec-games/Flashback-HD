/* Native room painter ($C2:1AD3).
 *
 * The original walks a list of records (desc, x, y), looks each descriptor up
 * in a table of block definitions, and expands the block's mask + 4bpp cells
 * into the 32x28-tile bitmap at $7E:2000. It is the hot spot of every room
 * load; this implementation follows the original instruction by instruction.
 *
 * Details (record format, block layout, clip rules and the right-edge
 * overdraw past $7E:9000) are documented in NOTES.md. Keep this file in sync
 * with that section if the painter is ever re-analysed.
 *
 * FLASHBACK_PAINT_NATIVE=0 delegates to the interpreter instead, for A/B
 * validation with FLASHBACK_FAST_LOAD=0. Unset (or any other value) uses the
 * native path. The helper charges no guest cycles on purpose: cutting the
 * black room-change pause is the point.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "cpu_state.h"
#include "paint_native.h"

extern int snes_frame_counter;

#define PAINT_BANK 0x7E
#define PAINT_BASE 0x2000
#define PAINT_COLUMN_STRIDE 0x380

/* 24-bit pointer addition; the low word carries into the bank. */
static uint32_t PtrAdd(uint32_t p, uint32_t n) {
  return (p + n) & 0xFFFFFFu;
}

/* The original's left/top pointer offsets use a 16-bit ADC and ignore the
 * carry into the bank ($C21CB4, $C21D2B); mirror that. */
static uint32_t PtrAddLow(uint32_t p, uint16_t n) {
  return (p & 0xFF0000u) | (uint16_t)((uint16_t)p + n);
}

static uint8_t Peek8(CpuState *cpu, uint32_t p) {
  return cpu_read8(cpu, (uint8_t)(p >> 16), (uint16_t)p);
}

static uint16_t Peek16(CpuState *cpu, uint32_t p) {
  return cpu_read16(cpu, (uint8_t)(p >> 16), (uint16_t)p);
}

/* Stream words are big-endian (LDA [$24] + XBA). */
static uint16_t Peek16Be(CpuState *cpu, uint32_t p) {
  const uint16_t v = Peek16(cpu, p);
  return (uint16_t)((v >> 8) | (v << 8));
}

/* 24-bit little-endian pointer in low RAM ($24/$26, $020D, ...). */
static uint32_t PeekPtr(CpuState *cpu, uint32_t p) {
  return (uint32_t)Peek8(cpu, p) |
         ((uint32_t)Peek8(cpu, PtrAdd(p, 1)) << 8) |
         ((uint32_t)Peek8(cpu, PtrAdd(p, 2)) << 16);
}

static void Poke16(CpuState *cpu, uint16_t addr, uint16_t v) {
  cpu_write16(cpu, PAINT_BANK, addr, v);
}

/* Draw one block. Returns false when clipping leaves nothing to draw; only a
 * drawn block updates X/Y, matching the original's early exits. */
static bool DrawBlock(CpuState *cpu, uint32_t q, uint16_t desc, int16_t x,
                      int16_t y, uint16_t *x_out, uint16_t *y_out) {
  int n = Peek8(cpu, q) + 1;
  int m = Peek8(cpu, PtrAdd(q, 1)) + 1;
  const uint16_t pixoff = Peek16(cpu, PtrAdd(q, 2));
  uint32_t mask = PtrAdd(q, 4);
  uint32_t pix = PtrAdd(mask, pixoff);

  int cols = n;
  int mask_skip = 0;
  int pix_skip = 0;

  if (desc & 0x8000) {
    if (x < 0) {
      const int w = n * 8 + x;
      if (w <= 0)
        return false;
      cols = w >> 3;
      const int c = n - cols;
      mask = PtrAddLow(mask, (uint16_t)c);
      pix = PtrAddLow(pix, (uint16_t)(c * 4));
      mask_skip = c;
      pix_skip = c * 4;
      x = 0;
    } else {
      if (x >= 256)
        return false;
      if (x + n * 8 > 256) {
        /* Deliberately under-clips; the overflow past $7E:9000 is real. */
        const int c = n >> 3;
        cols = n - c;
        mask_skip = c;
        pix_skip = c * 4;
      }
    }

    if (y < 0) {
      const int rows = y + m;
      if (rows <= 0)
        return false;
      const int skip = -y;
      const uint16_t cells = (uint16_t)(skip * n);
      mask = PtrAddLow(mask, cells);
      pix = PtrAddLow(pix, (uint16_t)(cells * 4));
      m = rows;
      y = 0;
    }

    if (y >= 224)
      return false;
    if (y + m > 224)
      m = 224 - y;
  }

  uint16_t dest = (uint16_t)(((uint16_t)x >> 3) * PAINT_COLUMN_STRIDE +
                             ((uint16_t)y >> 3) * 32 +
                             ((uint16_t)y & 7) * 2);
  int row_in_tile = y & 7;
  uint16_t src = 0;
  uint16_t x_last = dest;

  for (int row = 0; row < m; row++) {
    uint16_t xd = dest;
    for (int col = 0; col < cols; col++) {
      const uint8_t mb = Peek8(cpu, mask);
      mask = PtrAdd(mask, 1);
      if (mb != 0) {
        const uint16_t w0 = Peek16(cpu, PtrAdd(pix, src));
        const uint16_t w1 = Peek16(cpu, PtrAdd(pix, (uint16_t)(src + 2)));
        const uint16_t a0 = (uint16_t)(PAINT_BASE + xd);
        const uint16_t a1 = (uint16_t)(PAINT_BASE + 0x10 + xd);
        if (mb == 0xFF) {
          Poke16(cpu, a0, w0);
          Poke16(cpu, a1, w1);
        } else {
          const uint16_t bm = (uint16_t)(((mb ^ 0xFF) << 8) | (mb ^ 0xFF));
          Poke16(cpu, a0, (uint16_t)((cpu_read16(cpu, PAINT_BANK, a0) & bm) | w0));
          Poke16(cpu, a1, (uint16_t)((cpu_read16(cpu, PAINT_BANK, a1) & bm) | w1));
        }
      }
      src = (uint16_t)(src + 4);
      xd = (uint16_t)(xd + PAINT_COLUMN_STRIDE);
    }
    x_last = xd;
    mask = PtrAdd(mask, (uint32_t)mask_skip);
    src = (uint16_t)(src + pix_skip);
    if (++row_in_tile == 8) {
      row_in_tile = 0;
      dest = (uint16_t)(dest + 0x10);
    }
    dest = (uint16_t)(dest + 2);
  }

  *x_out = x_last;
  *y_out = src;
  return true;
}

static bool NativeEnabled(void) {
  static int enabled = -1;
  if (enabled < 0) {
    const char *v = getenv("FLASHBACK_PAINT_NATIVE");
    enabled = !(v && v[0] == '0' && v[1] == '\0');
  }
  return enabled != 0;
}

/* FLASHBACK_PAINT_DUMP also logs a hash of the painted bitmap per call, so
 * the native and delegated paths can be compared without depending on when
 * the host happens to dump a frame. */
static bool PaintDump(void) {
  static int on = -1;
  if (on < 0)
    on = getenv("FLASHBACK_PAINT_DUMP") != NULL;
  return on != 0;
}

static uint32_t PaintHash(CpuState *cpu) {
  uint32_t h = 2166136261u;
  for (uint32_t a = PAINT_BASE; a < PAINT_BASE + 0x7000; a++) {
    h ^= cpu_read8(cpu, PAINT_BANK, (uint16_t)a);
    h *= 16777619u;
  }
  return h;
}

static double NowMs(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}

static void LogPaint(const char *mode, uint32_t stream, uint32_t table,
                     uint16_t count, uint32_t before, uint32_t after,
                     uint64_t cycles, double ms) {
  fprintf(stderr,
          "[paint-hash] f=%d %s stream=$%06X table=$%06X count=%u "
          "pre=%08X post=%08X cycles=%llu ms=%.3f\n",
          snes_frame_counter, mode, stream, table, count, before, after,
          (unsigned long long)cycles, ms);
}

RecompReturn HlePaintRoom(CpuState *cpu) {
  const bool dump = PaintDump();
  const uint32_t stream = PeekPtr(cpu, (uint16_t)(cpu->D + 0x24));
  const uint32_t table = PeekPtr(cpu, 0x020D);
  const uint16_t count0 = Peek16Be(cpu, stream);
  uint16_t count = count0;
  const uint32_t before = dump ? PaintHash(cpu) : 0;
  const uint64_t cycles0 = cpu->cycles;
  const double wall0 = dump ? NowMs() : 0.0;

  /* The painter is entered M0/X0 by its only caller. Delegate anything else
   * (and the validation switch) to the original bytes. */
  if (!NativeEnabled() || cpu->m_flag || cpu->x_flag) {
    const RecompReturn r =
        interp_tier_run_call_frame(cpu, 0xC21AD3, 0xC219A7, 3, NULL);
    if (dump)
      LogPaint("interp", stream, table, count0, before, PaintHash(cpu),
               cpu->cycles - cycles0, NowMs() - wall0);
    return r;
  }

  uint32_t p = PtrAdd(stream, 2);
  uint16_t x_reg = cpu->X;
  uint16_t y_reg = cpu->Y;

  do {
    const uint16_t desc = Peek16Be(cpu, p);
    const int16_t x = (int16_t)Peek16Be(cpu, PtrAdd(p, 2));
    const int16_t y = (int16_t)Peek16Be(cpu, PtrAdd(p, 4));
    p = PtrAdd(p, 6);

    if (desc != 0xFFFF) {
      /* Y = (desc & $7FFF) * 4, wrapping in 16 bits like two ASLs. */
      const uint16_t yoff = (uint16_t)((desc & 0x7FFF) << 2);
      const uint32_t entry = PtrAdd(table, yoff);
      const uint32_t off = (uint32_t)Peek16(cpu, entry) |
                           ((uint32_t)Peek16(cpu, PtrAdd(entry, 2)) << 16);
      const uint32_t q = PtrAdd(table, off);
      DrawBlock(cpu, q, desc, x, y, &x_reg, &y_reg);
    }
  } while (--count != 0);

  /* The routine ends with LDA $128 / DEC A / BEQ RTL. */
  cpu->X = x_reg;
  cpu->Y = y_reg;
  cpu->A = 0;
  cpu->_flag_Z = 1;
  cpu->_flag_N = 0;
  cpu_mirrors_to_p(cpu);
  cpu->S = (uint16_t)(cpu->S + 3);
  if (dump)
    LogPaint("native", stream, table, count0, before, PaintHash(cpu),
             cpu->cycles - cycles0, NowMs() - wall0);
  return RECOMP_RETURN_NORMAL;
}
