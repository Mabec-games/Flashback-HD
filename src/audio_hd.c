/* Improved audio: the framework's S-DSP shadow mixer.
 *
 * snesrecomp can re-render the game's BRR voices with cubic interpolation in
 * float, instead of the S-DSP's Gaussian filter and per-voice 16-bit
 * truncation that muffle the treble (runner/src/snes/dsp_shadow.h). The
 * hardware mix keeps running and is the reference: the shadow is heard only
 * once a verifier has proven it tracks the hardware, and it reverts to the
 * hardware mix (logging DEGRADED) the moment it stops matching.
 *
 * Flashback HD turns it on by default.
 *
 * Environment:
 *   FLASHBACK_HD_AUDIO=0       keep the hardware mix
 *   SNESRECOMP_AUDIO_SHADOW    the framework switch; set explicitly, it wins
 *   FLASHBACK_AUDIO_STATS=1    print the verifier's state once a second
 */

#include <stdio.h>
#include <stdlib.h>

#include "audio_hd.h"
#include "common_rtl.h"
#include "snes/apu.h"
#include "snes/dsp.h"
#include "snes/dsp_shadow.h"
#include "snes/snes.h"

extern Snes *g_snes;  /* the runner's machine; no header declares it */

static int g_stats = -1;
static unsigned g_frames, g_proven_frames;

void AudioHdOnRomLoaded(void) {
  const char *hd = getenv("FLASHBACK_HD_AUDIO");
  /* Before SnesInit, which creates the DSP and reads the switch. */
  setenv("SNESRECOMP_AUDIO_SHADOW", hd && hd[0] == '0' ? "0" : "1", 0);
}

static const ShadowVerifier *Verifier(void) {
  if (!g_snes || !g_snes->apu || !g_snes->apu->dsp)
    return NULL;
  const DspShadow *shadow = (const DspShadow *)g_snes->apu->dsp->shadow;
  return shadow && shadow->enabled ? &shadow->vf : NULL;
}

void AudioHdAfterRunFrame(void) {
  if (g_stats < 0) {
    const char *v = getenv("FLASHBACK_AUDIO_STATS");
    g_stats = v && v[0] && v[0] != '0';
  }
  if (!g_stats)
    return;
  const ShadowVerifier *vf = Verifier();
  if (!vf)
    return;
  g_frames++;
  g_proven_frames += vf->proven;
  if (g_frames % 60 == 0)
    fprintf(stderr,
            "[audio_hd] frame %d: %s, proven %u%% of frames so far, gain %.3f, "
            "correlation %.3f, pauses %llu\n",
            snes_frame_counter, vf->proven ? "ENHANCED" : "hardware",
            g_proven_frames * 100u / g_frames, vf->gain, vf->last_r,
            (unsigned long long)vf->pauses);
}
