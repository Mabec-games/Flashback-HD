# Cutscene renderer (SNES USA)

Found 2026-10-17 by profiling the interpreter over frames 1700-2100 (corridor
chase in the intro) and disassembling the hot code. All in bank $C1.

## Verdict

Cutscenes are **real-time polygon rendering**, not pre-rendered frames. The
CPU rasterizes flat-coloured polygons and ellipses into a 4bpp tiled bitmap in
WRAM and DMAs it to VRAM. A keyframe is produced every 9 frames (~6.7 fps).

## Pipeline

| Stage | Address | What it does |
|---|---|---|
| Edge setup | `$C11309` (called 24x from `$C1:0000-$1200`) | slope via hardware divider `$4204/$4206/$4214` |
| Edge walker | `$C10104` loop | fixed-point DDA, writes `(x_left, x_right)` per scanline via `[$24]` |
| Ellipse | `$C14F0A` (fill loop to `$C15641`) | centre stored to `$1E7E/$1E7C`, per-row spans via `JSR $5691` |
| Span renderer | `$C113C0` (entry) | span list at `$00:0432`: `db y` (bit7 = end), then `dw x1, x2` pairs |
| Span fill | `$C113DB-$C11541` | writes 4bpp planar tiles at `$7E:2000+`; row base table `$C1:8215`, edge masks `$C1:81D3/$81E5`, colour planes `$C1:8193/$8195` indexed by colour `$1E52` |
| Upload | DMA ch0 `$7E:6000 -> $2118` | dirty tiles to VRAM `$2000-$2FFF` (BG1/BG2 linear tilemaps = framebuffer) |
| Idle wait | `$C15971` | spins on `$1E4E` until NMI |

`$1E64 != 0` skips span rendering at `$C113C0` (branch to `$C11542`).

## Shape records (decoder at `$C10003`)

Read through the long pointer `[$20]` (from `$2C/$2E`); origin added from
`$18` (x) / `$1C` (y). First byte:

- bit 7 set: a filled **ellipse**, 4 big-endian words `x y rx ry` (centre
  plus origin, radii), pushed for `JSR $C14F0A` (stack: x S+3, y S+5,
  rx S+7, ry S+9; midpoint algorithm, ends in RTS)
- `0`: a point (big-endian `x y`), drawn by `JSL $C10153`
- `n`: polygon of `n+1` vertices: first vertex big-endian words, then signed
  byte `(dx, dy)` deltas (a zero `dy` has a run-length special case). The
  absolute vertices go to `$00:02A2` as word pairs, count in dp `$14`.

`JSL $C101C2` (from `$C10127`) then builds edge tables, clips to the 240x128
viewport and sets the rasterizer in dp `$24`; `JSL $C10000` = `JMP ($0024)`.
Colour `$1E52` is index*4; the bit-plane table at `$C0:8193` (DB=$00) is the
identity, so pixel value = colour index.

## Layers and keyframes

One bitmap at `$7E:2000`, uploaded whole (16 KB DMA) after each batch:

- to VRAM `$5000` = BG2, palette 2: the static scene background, drawn once
  (e.g. frame 1396 for the corridor scene)
- to VRAM `$1000` = BG1, palette 1: the animated foreground, redrawn from a
  cleared bitmap every keyframe (every 5-10 frames); colour 0 transparent

BG1 scroll is (-8, -48); on screen the viewport origin is (8, 47) because a
BG line shows scroll row y + vscroll + 1. The game runs at INIDISP
brightness 14.

## Capture (done)

`src/cutscene_hd.c` captures passively through the interpreter PC hook
(`g_interp_bridge_pc_hook`): at `$C10127` (polygon, vertex list ready) and
`$C10073` (ellipse, recomputed from the record at `[$20]` + origin), reading
guest memory directly (no `cpu_read*`, which latches open bus). Every VRAM
DMA is logged for layer attribution. `FLASHBACK_POLY_LOG=<file>` writes it all
out.

Wrapping `$C101C2` / `$C14F0A` with `hle_func` was tried first and **changed
the game**: 8 of 25 intro frames differed from the plain build, because the
detour's cycle accounting shifts the cinematic engine's keyframe pacing. The
passive hook leaves all 25 identical. Bank `$C1` must stay interpreted for
the hook to fire (`recomp/bankc1.cfg` is deliberately empty).

Verified complete over the tank scene (frames 2600-2760): 822 entries to
`$C101C2` and 822 polygons logged; 54 to `$C14F0A`, 54 ellipses.

`polyscene.py` rebuilds a frame from the log: 97% of pixels match the game
at 1x (the rest is edge rounding), and it renders the same scene at any
scale.

## Still to do

- Points (`$C10153`) are not captured (rare; shown natively).
- Edge rules: match the SNES span inclusivity to close the last 3%.
- Palette changes mid-keyframe: the HD redraw uses the palette live at
  presentation, which is right unless a scene changes CGRAM between drawing
  and showing a keyframe.
