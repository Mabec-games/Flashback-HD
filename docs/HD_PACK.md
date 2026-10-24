# HD pack

Replacement art is loaded at runtime from a pack folder, never compiled in.
Default location: `hd/` next to the executable. Override with
`FLASHBACK_HD_PACK=/path/to/pack`.

| File | Used for |
|---|---|
| `title/background.png` | Title screen background, any size; drawn to fill the 16:9 frame (keep it 16:9). |
| `title/logo.png` | Title logo, RGBA. Fades in over 30 frames when the menu appears. Its opaque part is placed at x 48.7%-94.0% of the frame width, top at 20.6%, keeping its proportions. |

Missing files fall back to the original screen. The menu itself always comes
from the game (BG3, live palette), so selection and blinking keep working.

## Settings (environment)

| Variable | Effect |
|---|---|
| `FLASHBACK_HD_SCALE` | 2-8, default 4: presentation resolution, 398x224 times the scale (16:9). |
| `FLASHBACK_HD_AUDIO=0` | Keep the hardware S-DSP mix. By default the cubic-interpolated shadow mix is used once it has proven itself against the hardware mix (about 10-20 s in), and it falls back on any divergence. |
| `FLASHBACK_AUDIO_STATS=1` | Print the audio verifier's state once a second. |
| `FLASHBACK_POLY_LOG=<file>` | Log captured cutscene shapes and VRAM DMAs (research). |
| `FLASHBACK_WIDE_ROOMS=0` | Plain black side bars during gameplay instead of the neighbouring rooms. |
| `FLASHBACK_CUTSCENE_FRAMES=N` | Cutscene keyframes take at most N frames (6 is the PC pace; default 0 keeps the SNES pace). The engine never waits for its music, so a faster picture runs ahead of the sound. |
| `FLASHBACK_FAST_LOAD=0` | Keep the original loading pauses. By default blanked loading screens (room changes, level loads) run the game faster within a per-frame time budget, so they end sooner without audio stutter. `=2` logs each frame. |
| `FLASHBACK_ROOM_EDGES=0` | Keep the original room transition line at the edge of the SNES field. By default rooms change at the edges of the 16:9 screen (the game itself is patched for this). |
| `FLASHBACK_ROOM_LOG=1` or `=<file>` | Development: log Conrad's position before and after each room change, to the terminal or appended to a file. |
| `FLASHBACK_SEAM_BLEND=0` | Show every join between neighbouring rooms as painted. By default joins whose art does not continue are dithered over. |
| `FLASHBACK_ROOM_LABELS=1` | Development: show room numbers (current in yellow, neighbours in grey). |
| `FLASHBACK_WIDE_DUMP=<dir>` | Development: write each cached room image to the directory. |
| `FLASHBACK_WIDE_DEBUG=1` | Log room caching, speculative neighbour rendering and its cost. |
| `FLASHBACK_WATCH=0231,1E72` | Research: log the instruction that changes each listed WRAM byte. |
