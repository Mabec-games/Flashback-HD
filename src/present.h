#ifndef PRESENT_H
#define PRESENT_H

#include <stddef.h>
#include <stdint.h>

#include "display_aspect.h"

/* 16:9 presentation: the SnesDesktopHostGame presenter hooks. See present.c. */
int FlashbackPresentScale(void);
void FlashbackPrepareFrame(int drawable_w, int drawable_h, int *frame_w,
                           int *frame_h);
int FlashbackWindowBaseWidth(int frame_w);
void FlashbackComputeViewport(int frame_w, int frame_h, int drawable_w,
                              int drawable_h, SnesDisplayViewport *viewport);
int FlashbackDrawFrame(uint8_t *dst, size_t pitch, const uint8_t *field,
                       int frame_w, int frame_h, double alpha);

/* Native field columns shown by each side area at the configured aspect. */
int FlashbackSideColumns(void);

#endif /* PRESENT_H */
