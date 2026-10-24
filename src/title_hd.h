#ifndef TITLE_HD_H
#define TITLE_HD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* HD title screen from the hd/title pack images. See title_hd.c. */

void TitleHdOnReset(void);
/* While the title screen is shown, draw it into the canvas_w x canvas_h ARGB
 * frame; the native field would occupy field_w columns from field_x. False
 * when the title is not on screen or the pack images are unavailable. */
bool TitleHdDraw(uint8_t *dst, size_t pitch, int canvas_w, int canvas_h,
                 int field_x, int field_w);

#endif /* TITLE_HD_H */
