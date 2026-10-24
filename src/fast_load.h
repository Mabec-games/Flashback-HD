#ifndef FAST_LOAD_H
#define FAST_LOAD_H

/* Shorter loading pauses (fast_load.c). Around each guest frame, after
 * any speculative frames (the budget covers the guest frame alone). */
void FastLoadBeforeRunFrame(void);
void FastLoadAfterRunFrame(void);

#endif /* FAST_LOAD_H */
