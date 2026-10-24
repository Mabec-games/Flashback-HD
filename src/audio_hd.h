#ifndef AUDIO_HD_H
#define AUDIO_HD_H

/* Improved audio through the S-DSP shadow mixer. See audio_hd.c. */

/* Before SnesInit: choose whether the shadow mixer is armed. */
void AudioHdOnRomLoaded(void);
/* After each guest frame: optional verifier statistics. */
void AudioHdAfterRunFrame(void);

#endif /* AUDIO_HD_H */
