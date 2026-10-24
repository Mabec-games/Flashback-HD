/* Research: which instruction writes a WRAM byte.
 *
 * FLASHBACK_WATCH=0231,0232,...  (WRAM offsets, hex, up to 16)
 * After every interpreted opcode, compare the watched bytes with their last
 * value; on a change, log the opcode that ran (its PC) with old and new
 * values to stderr. Off and free when the variable is unset. Only interpreted
 * code is seen, which is all of Flashback's game code today.
 *
 * FLASHBACK_PAINT_DUMP=1 dumps the arguments of the room-painting call at
 * $C2:19A7 (the JSL into the routine at $C21AD3): the room record pointer,
 * the resolved command stream and its first bytes. Research only.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common_rtl.h"
#include "cpu_state.h"
#include "research_watch.h"
#include "snes/interp_bridge.h"

enum { kMaxWatch = 16 };

static unsigned g_addr[kMaxWatch];
static unsigned char g_last[kMaxWatch];
static int g_count;
static uint32_t g_prev_pc;
static void (*g_chained)(uint32_t, int, int);

static void OnOpcode(uint32_t pc24, int m_flag, int x_flag) {
  for (int i = 0; i < g_count; i++) {
    const unsigned char now = g_cpu.ram[g_addr[i]];
    if (now != g_last[i]) {
      fprintf(stderr, "[watch] frame %d $%05X %02X -> %02X by opcode at $%06X\n",
              snes_frame_counter, g_addr[i], g_last[i], now, g_prev_pc);
      g_last[i] = now;
    }
  }
  g_prev_pc = pc24;
  if (g_chained)
    g_chained(pc24, m_flag, x_flag);
}

/* ── room-painting call dump (FLASHBACK_PAINT_DUMP) ─────────────────────── */

static uint32_t ReadPtr(CpuState *cpu, uint16_t dp) {
  uint32_t p = 0;
  for (int i = 0; i < 3; i++)
    p |= (uint32_t)cpu_read8(cpu, 0, (uint16_t)(dp + i)) << (8 * i);
  return p;
}

static void DumpBytes(CpuState *cpu, uint32_t pc24, int n) {
  for (int i = 0; i < n; i += 16) {
    fprintf(stderr, "  $%06X:", (unsigned)(pc24 + i));
    for (int j = 0; j < 16 && i + j < n; j++)
      fprintf(stderr, " %02X", cpu_read8(cpu, (uint8_t)((pc24 + i + j) >> 16),
                                         (uint16_t)(pc24 + i + j)));
    fprintf(stderr, "\n");
  }
}

static void OnPaintCall(CpuState *cpu, uint32_t pc24) {
  (void)pc24;
  const uint16_t d = cpu->D;
  const uint32_t rec = ReadPtr(cpu, (uint16_t)(d + 0x20));
  const uint32_t stream = ReadPtr(cpu, (uint16_t)(d + 0x24));
  const uint32_t table = ReadPtr(cpu, 0x020D);
  fprintf(stderr,
          "[paint] frame %d rec=$%06X stream=$%06X table=$%06X "
          "D=$%04X M%d X%d A=$%04X Xr=$%04X Y=$%04X S=$%04X PB=$%02X DB=$%02X\n",
          snes_frame_counter, rec, stream, table, d, cpu->m_flag, cpu->x_flag,
          cpu->A, cpu->X, cpu->Y, cpu->S, cpu->PB, cpu->DB);
  {
    const uint32_t h = rec + 10;
    const unsigned off = ((unsigned)cpu_read8(cpu, (uint8_t)(h >> 16), (uint16_t)h) << 8) |
                         cpu_read8(cpu, (uint8_t)((h + 1) >> 16), (uint16_t)(h + 1));
    fprintf(stderr, "[paint] record+10 (big endian) = $%04X -> stream check $%06X\n",
            off, (rec & 0xFF0000) | (((rec & 0xFFFF) + off) & 0xFFFF));
  }
  fprintf(stderr, "[paint] stream bytes:\n");
  DumpBytes(cpu, stream, 128);
  fprintf(stderr, "[paint] table bytes:\n");
  DumpBytes(cpu, table, 32);
}

static void OnPaintClip(CpuState *cpu, uint32_t pc24) {
  (void)pc24;
  const uint16_t d = cpu->D;
  unsigned v[8];
  static const uint8_t offs[8] = {0x00, 0x04, 0x08, 0x0C, 0x10, 0x18, 0x1C, 0x1E};
  for (int i = 0; i < 8; i++) {
    const uint16_t a = (uint16_t)(d + offs[i]);
    v[i] = cpu_read8(cpu, 0, a) | ((unsigned)cpu_read8(cpu, 0, (uint16_t)(a + 1)) << 8);
  }
  fprintf(stderr, "[clip] frame %d x=%u y=%d w=%u h=%u $10=%u $18=%u $1C=%u $1E=%u\n",
          snes_frame_counter, v[0], (int16_t)v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
  if (snes_frame_counter == 4793 && v[0] == 224 && v[1] == 10) {
    fprintf(stderr, "[clip] A=%04X X=%04X Y=%04X S=%04X P=%02X PB=%02X DB=%02X D=%04X\n",
            cpu->A, cpu->X, cpu->Y, cpu->S, cpu->P, cpu->PB, cpu->DB, cpu->D);
    for (int i = 0; i < 0x2C; i += 0x10)
      fprintf(stderr, "[clip] dp%02X: %02X %02X %02X %02X %02X %02X %02X %02X  "
                      "%02X %02X %02X %02X %02X %02X %02X %02X\n", i,
              cpu_read8(cpu, 0, (uint16_t)(d + i + 0)), cpu_read8(cpu, 0, (uint16_t)(d + i + 1)),
              cpu_read8(cpu, 0, (uint16_t)(d + i + 2)), cpu_read8(cpu, 0, (uint16_t)(d + i + 3)),
              cpu_read8(cpu, 0, (uint16_t)(d + i + 4)), cpu_read8(cpu, 0, (uint16_t)(d + i + 5)),
              cpu_read8(cpu, 0, (uint16_t)(d + i + 6)), cpu_read8(cpu, 0, (uint16_t)(d + i + 7)),
              cpu_read8(cpu, 0, (uint16_t)(d + i + 8)), cpu_read8(cpu, 0, (uint16_t)(d + i + 9)),
              cpu_read8(cpu, 0, (uint16_t)(d + i + 10)), cpu_read8(cpu, 0, (uint16_t)(d + i + 11)),
              cpu_read8(cpu, 0, (uint16_t)(d + i + 12)), cpu_read8(cpu, 0, (uint16_t)(d + i + 13)),
              cpu_read8(cpu, 0, (uint16_t)(d + i + 14)), cpu_read8(cpu, 0, (uint16_t)(d + i + 15)));
  }
}

static void OnPaintStore(CpuState *cpu, uint32_t pc24) {
  (void)pc24;
  if (cpu->X >= 0x7000)
    fprintf(stderr, "[oob] frame %d store pc=$%06X X=$%04X A=$%04X dp00=%04X dp04=%04X\n",
            snes_frame_counter, pc24, cpu->X, cpu->A,
            cpu_read8(cpu, 0, 0) | (cpu_read8(cpu, 0, 1) << 8),
            cpu_read8(cpu, 0, 4) | (cpu_read8(cpu, 0, 5) << 8));
}

static void OnPaintRowEnd(CpuState *cpu, uint32_t pc24) {
  (void)pc24;
  const uint16_t d = cpu->D;
  const unsigned clip = cpu_read8(cpu, 0, (uint16_t)(d + 0x1C)) |
                        (cpu_read8(cpu, 0, (uint16_t)(d + 0x1D)) << 8);
  if (!clip)
    return;
  const unsigned mask = (unsigned)cpu_read8(cpu, 0, (uint16_t)(d + 0x28)) |
                        ((unsigned)cpu_read8(cpu, 0, (uint16_t)(d + 0x29)) << 8) |
                        ((unsigned)cpu_read8(cpu, 0, (uint16_t)(d + 0x2A)) << 16);
  const unsigned pix = (unsigned)cpu_read8(cpu, 0, (uint16_t)(d + 0x30)) |
                       ((unsigned)cpu_read8(cpu, 0, (uint16_t)(d + 0x31)) << 8) |
                       ((unsigned)cpu_read8(cpu, 0, (uint16_t)(d + 0x32)) << 16);
  const unsigned rows = cpu_read8(cpu, 0, (uint16_t)(d + 0x0C)) |
                        (cpu_read8(cpu, 0, (uint16_t)(d + 0x0D)) << 8);
  fprintf(stderr, "[row] frame %d left=%u rowsleft=%u mask=$%06X pix=$%06X Y=%04X $2C=%04X\n",
          snes_frame_counter, clip, rows, mask, pix, cpu->Y,
          cpu_read8(cpu, 0, (uint16_t)(d + 0x2C)) |
              (cpu_read8(cpu, 0, (uint16_t)(d + 0x2D)) << 8));
}

void ResearchWatchInstall(void) {
  if (getenv("FLASHBACK_PAINT_DUMP")) {
    interp_bridge_add_pre_opcode_hook(0xC219A7, OnPaintCall);
    interp_bridge_add_pre_opcode_hook(0xC21D5F, OnPaintClip);
    interp_bridge_add_pre_opcode_hook(0xC21DEF, OnPaintRowEnd);
    fprintf(stderr, "[paint] dumping room-painting calls\n");
  }
  const char *spec = getenv("FLASHBACK_WATCH");
  if (!spec || !*spec)
    return;
  char buf[256];
  snprintf(buf, sizeof buf, "%s", spec);
  for (char *tok = strtok(buf, ","); tok && g_count < kMaxWatch; tok = strtok(NULL, ","))
    g_addr[g_count++] = (unsigned)strtoul(tok, NULL, 16) & 0x1FFFF;
  /* Chain to whatever hook is already installed (the cutscene capture). */
  g_chained = g_interp_bridge_pc_hook;
  g_interp_bridge_pc_hook = OnOpcode;
  fprintf(stderr, "[watch] %d WRAM byte(s) watched\n", g_count);
}
