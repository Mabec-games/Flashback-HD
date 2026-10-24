/* The host lifecycle hooks, fanned out to each Flashback HD feature. */

#include "audio_hd.h"
#include "cutscene_hd.h"
#include "fast_load.h"
#include "flashback_hd.h"
#include "research_watch.h"
#include "room_edges.h"
#include "rooms_wide.h"
#include "title_hd.h"

void FlashbackOnRomLoaded(const uint8_t *rom, size_t size) {
  AudioHdOnRomLoaded();
  CutsceneHdOnRomLoaded(rom, size);
  ResearchWatchInstall();  /* last: it chains the cutscene PC hook */
}

/* The guest timeline jumped: forget what was captured or animating. */
void FlashbackOnReset(void) {
  CutsceneHdOnReset();
  TitleHdOnReset();
  RoomsWideOnReset();
}

void FlashbackBeforeRunFrame(void) {
  CutsceneHdBeforeRunFrame();
  RoomEdgesApply();
  RoomsWideBeforeRunFrame();
  FastLoadBeforeRunFrame();  /* last: it times the guest frame */
}

void FlashbackAfterRunFrame(const SnesDesktopHostFrameStats *stats) {
  (void)stats;
  FastLoadAfterRunFrame();  /* first, for the same reason */
  AudioHdAfterRunFrame();
  RoomsWideAfterRunFrame();
  RoomEdgesAfterRunFrame();
}
