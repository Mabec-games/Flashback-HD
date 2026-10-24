#ifndef FLASHBACK_HD_H
#define FLASHBACK_HD_H

#include <stddef.h>
#include <stdint.h>

#include "host_main.h"

/* SnesDesktopHostGame lifecycle hooks shared by the HD features. */
void FlashbackOnRomLoaded(const uint8_t *rom, size_t size);
void FlashbackOnReset(void);
void FlashbackBeforeRunFrame(void);
void FlashbackAfterRunFrame(const SnesDesktopHostFrameStats *stats);

#endif /* FLASHBACK_HD_H */
