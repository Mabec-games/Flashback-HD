/* Shorter loading pauses.
 *
 * Changing room blanks the screen for about 100 frames (1.7 s) while the
 * game builds the room's painting on the CPU (the masked tile blit at
 * $C21B90 is half of it), then fades in over 15 frames. The work is
 * CPU-bound in guest time, so while the screen is blanked outside a
 * cutscene, interpreted code is charged a fraction of its cycles and the
 * load finishes in fewer frames. The same work then lands in fewer host
 * frames, so the fraction adapts to keep each guest frame within a host
 * time budget: the sound never starves, and a faster PC gets shorter
 * pauses. Level loads and other blanked screens are shortened the same way.
 *
 * Environment:
 *   FLASHBACK_FAST_LOAD=0   keep the original pauses
 *   FLASHBACK_FAST_LOAD=2   also log each blanked frame's divisor and cost
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "common_rtl.h"
#include "cutscene_hd.h"
#include "fast_load.h"
#include "snes/interp_bridge.h"
#include "snes/ppu.h"
#include "desktop/sdl_compat.h"

static const double kBudgetMs = 10.0;  /* host time per guest frame */
enum { kMaxDivisor = 16 };

static int g_enabled = -1;
static int g_divisor = 4;
static bool g_active;
static uint64_t g_start;

void FastLoadBeforeRunFrame(void) {
  if (g_enabled < 0) {
    const char *v = getenv("FLASHBACK_FAST_LOAD");
    g_enabled = v && *v ? atoi(v) : 1;
  }
  const Ppu *ppu = g_ppu;
  g_active = g_enabled && ppu && PPU_forcedBlank(ppu) && !CutsceneHdLayersOnScreen();
  if (!g_active)
    return;
  g_interp_fast_all = true;
  g_interp_fast_divisor = (uint8_t)g_divisor;
  g_start = SDL_GetPerformanceCounter();
}

void FastLoadAfterRunFrame(void) {
  if (!g_active)
    return;
  g_interp_fast_all = false;
  g_interp_fast_divisor = 0;
  const double ms = (double)(SDL_GetPerformanceCounter() - g_start) * 1000.0 /
                    (double)SDL_GetPerformanceFrequency();
  if (g_enabled > 1)
    fprintf(stderr, "[fast_load] frame %d: divisor %d, %.1f ms\n", snes_frame_counter,
            g_divisor, ms);
  /* Step towards the budget: up while comfortably under it, down when
   * over (frames carry a fixed cost besides the guest's work). */
  if (ms > kBudgetMs)
    g_divisor -= ms > 1.5 * kBudgetMs ? 2 : 1;
  else if (ms < 0.8 * kBudgetMs)
    g_divisor++;
  g_divisor = g_divisor < 1 ? 1 : g_divisor > kMaxDivisor ? kMaxDivisor : g_divisor;
}
