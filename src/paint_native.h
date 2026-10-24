#ifndef PAINT_NATIVE_H
#define PAINT_NATIVE_H

#include "cpu_state.h"

/* Native replacement for the room painter at $C2:1AD3.
 *
 * Expands the calling room's command stream into the 4bpp bitmap at
 * $7E:2000. See NOTES.md ("native room painter") for the format and the
 * clipping rules. With FLASHBACK_PAINT_NATIVE=0 the helper delegates to the
 * interpreter instead, for differential validation. */
RecompReturn HlePaintRoom(CpuState *cpu);

#endif /* PAINT_NATIVE_H */
