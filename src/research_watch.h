#ifndef RESEARCH_WATCH_H
#define RESEARCH_WATCH_H

/* Research: log the instruction that changes a watched WRAM byte
 * (FLASHBACK_WATCH). Install after the other interpreter hooks. */
void ResearchWatchInstall(void);

#endif /* RESEARCH_WATCH_H */
