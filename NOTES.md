# Flashback HD – work notes

Working notes, latest first. Settings are documented in `docs/HD_PACK.md`,
engine reference in `tools/research/`.

## Decisions (user)

- **Room transitions are final – do not change unless asked.** View always
  centred; rooms change at the 16:9 screen edges; Conrad enters just inside
  the screen edge he came from (`src/room_edges.c`).
- **No upscale filters** (Scale2x in gameplay and a crisp xBR prototype on
  cutscene backgrounds were both rejected): keep the original pixel art.
- **Cutscenes stay at the SNES pace.** `FLASHBACK_CUTSCENE_FRAMES=6/8` was
  tried: the engine never waits for its music, so a faster picture runs
  ahead of the sound. Option kept, default off.
- Widescreen-only build; gameplay neighbours shown in the side areas.

## Done: native room painter (`$C2:1AD3`)

Analysed 2026-10-23, implemented and validated 2026-10-24. Everything below
was checked against the ROM disassembly and headless level-1 runs with
`FLASHBACK_PAINT_DUMP=1 FLASHBACK_FAST_LOAD=0`. `src/paint_native.c` now
replaces the painter via `hle_func`; the rest of bank `$C2` stays interpreted.

### Why

The painter calls per room load (three for level-1 room 1) are the bulk of the
~101 black frames (`$C21B90-$C21C00` is ~51% of interpreted opcodes).
Reimplementing the routine in C with `hle_func`, charging no guest cycles,
should leave only the DMA and a few setup frames. The bitmap at `$7E:2000`
must stay byte-identical.

### Call site and arguments

- `JSL $C21AD3` from `$C2:19A7`; three calls for level-1 room 1 (observed at
  frames 4793, 4837, 4852 in one run; 4793, 4818, 4898 in another).
- Entry state: `D=$0000`, M=0, X=0, DB=0, PB=$C2.
- `$24/$26` = 24-bit pointer to the command stream (WRAM low RAM, e.g.
  `$0002B2`). `$020D` holds the 24-bit LE table pointer (observed `$CC900F`;
  the bank byte is at `$020F`).
- `$20/$22` (room record) is not used by the painter; the caller builds the
  stream from it before the call.
- On return the caller sets `$20/$22 = $7E:2000`, `$24 = $2800`, `$1C =
  $7000` and calls `$C2CC06` to DMA `$7E:2000..$8FFF` to VRAM `$2800`.
  The painter's exit scratch (dp `$00..$32`, A/X/Y) is not read afterwards.
- Ends with `RTL`: the helper must `cpu->S += 3`. It leaves A=0, Z=1, N=0,
  M/X unchanged (0), D/DB/PB unchanged. `$0128` (record counter) ends at 1
  (the last `DEC` is not stored); the caller does not read it.

### Stream

Big-endian words:

```
u16 count;                              // read at $24; stream += 2
count x { u16 desc; s16 x; s16 y; }     // each record: stream += 6
```

- `desc == $FFFF` skips the record (`$C21C27`).
- `desc & $8000` selects the clipping variant at `$C21C20`; otherwise the
  unclipped variant at `$C21B18`.
- `index = desc & $7FFF`; `Q = table_base + u32_le(table_base + 4*index)`.
- The loop is do/while (`$C21E1E`): process a record, then decrement the
  16-bit count and finish at zero. Do not turn it into a plain `for`, or a
  count of 0 differs.

### Runtime observations (level-1 room 1)

- Stream: 153 records, 32 with bit 15; every `x` is a multiple of 8; `x`/`y`
  include negative values (e.g. x=-8, y=-2).
- Table `$CC900F`. Example: `desc=$803E x=208 y=72` -> index 62 -> block
  `$CD54AF`: n=10, m=8, pixoff=80, mask at `$CD54B3`, cells at `$CD5503`.
  Right clip drops 1 column (`$18=1`, `$1C=4`), draws 9 and overdraws.

### Block at Q

```
u8  n1;             // n = n1 + 1 tile columns (8 px each)
u8  m1;             // m = m1 + 1 cell rows (8 px each)
u16 pixoff;         // LE; == n*m
u8  mask[n*m];      // Q+4
u8  cells[n*m][4];  // Q+4+pixoff
```

- One `cells` entry is one 8-pixel row: word 0 at `+0` is planes 0/1, word 1
  at `+2` is planes 2/3.
- Destination `$7E:2000`, 4bpp planar, 32 tile columns x 28 tile rows,
  `$380` bytes per column, second plane-pair word at `X+$10`:
  `dest = (x>>3)*$380 + (y>>3)*32 + (y&7)*2`.

### Render core (both variants)

```
for each of m rows:
  X = dest
  for each of cols columns:
    mb = *mask++
    if (mb == 0) { src += 4; continue; }              // transparent
    w0 = u16(pix[src+0]); w1 = u16(pix[src+2]);
    if (mb == $FF) {                                  // opaque
      store16($7E2000+X, w0); store16($7E2010+X, w1);
    } else {
      bm = ((mb^$FF)<<8) | (mb^$FF);
      store16($7E2000+X, (load16($7E2000+X) & bm) | w0);
      store16($7E2010+X, (load16($7E2010+X) & bm) | w1);
    }
    src += 4; X += $380;                              // 16-bit, wraps
  // row end
  mask += mask_skip; src += pix_skip;
  if (++row_in_tile == 8) { row_in_tile = 0; dest += $10; }
  dest += 2;
}
```

The non-opaque case is a bitwise select, not an arithmetic blend:
`out = (dst & inverse_mask) | src`.

### Variant 1 (desc bit 15 clear, `$C21B18`)

- `cols = n`, `mask_skip = 0`, `pix_skip = 0`; no clipping at all.
- Used for blocks that are fully on screen.

### Variant 2 (desc bit 15 set, `$C21C20`)

Start with `cols = n`, `mask_skip = 0`, `pix_skip = 0`.

- **Left** (`x < 0`): `w = n*8 + x` (signed). If `w <= 0`, draw nothing.
  Otherwise `cols = w>>3`; `c = n - cols`; `mask += c`; `pix += c*4`;
  `mask_skip = c`; `pix_skip = c*4`; `x = 0`.
- **Right** (`x >= 0`): `x >= 256` skips the record (`$C21CC8`). Otherwise,
  if `x + n*8 > 256`, `c = n>>3`; `cols = n - c`; `mask_skip = c`;
  `pix_skip = c*4`.
  This under-clips: 9 columns at x=208 drop only 1, leaving the right edge at
  272. The overdraw past `$7E:9000` is real and must be reproduced. In level
  1 it reaches X~`$7A7E`, i.e. `$7E:9A7E`, outside the `$7000` DMA.
- **Top** (`y < 0`): `rows = y + m` (signed). If `rows <= 0`, draw nothing.
  Otherwise `skip = -y`; `mask += skip*n`; `pix += skip*n*4`; `m = rows`;
  `y = 0`. Mirror the signed/zero/carry test at `$C21D09`/`$C21D0E`.
- **Bottom**: if `y >= 224`, draw nothing; else if `y + m > 224`,
  `m = 224 - y`.
- Only x/y are clipped; `n`/`m` stay in the header.

### Quirks

- `INC $28; BNE; INC $2A` advances the mask pointer by one. With M=0 the
  65816 RMW width is 16-bit, so this is a correct 24-bit increment (the row
  trace crosses `$CD54FF -> $CD5500` without corruption). It is not an 8-bit
  page bug.
- `m` is not a multiple of 8 (seen: 17, 40, 88); the row-in-tile counter and
  the `$10` hop handle that.
- Destination stores wrap modulo 64K within bank `$7E`; `$7E2010+X` can wrap
  independently.
- Source reads are 16-bit at `pix+Y`; cell offsets are linear across the
  whole block.
- The `$7000`-byte buffer is cleared by the caller before painting; the
  painter never clears.

### HLE plan

1. `recomp/bankc2.cfg`:

   ```
   bank = c2
   func RoomPaint 1AD3 exit_mx:0,0
   hle_func 1AD3 HlePaintRoom
   ```

   The `func` is only for a readable stub name; the rest of bank `$C2` stays
   interpreted. `hle_func` is reachable because the painter is entered by a
   `JSL` (interpreted jumps do not bounce).
2. `bash tools/regen.sh`; check that generated callers name the stub.
3. Implement `HlePaintRoom` in `src/paint_native.c` (+ header, add to
   `CMakeLists.txt`), or in `src/gen_stubs.c`. Use `cpu_read8` for guest
   reads and direct `cpu->ram[]` writes for `$7E:2000`.
4. Helper: parse, paint, then `cpu->S += 3`; A=0, Z=1, N=0; leave M/X 0,
   D/DB/PB; charge 0 cycles.
5. `FLASHBACK_PAINT_NATIVE=0` delegates to the interpreter and returns its
   result unchanged:
   `interp_tier_run_call_frame(cpu, 0xC21AD3, 0xC219A7, 3, NULL)`.
   The interpreter pops the frame; do not pop again.
6. Keep the env switch until the differential run passes.

### Validation (passed)

- Delegated (`FLASHBACK_PAINT_NATIVE=0`) vs native, same script and
  `FLASHBACK_FAST_LOAD=0`. At frame 5092 the level-1 dump has **0 differing
  bytes** in WRAM `$2000..$8FFF` (the painted buffer) and in VRAM
  `$2800..$97FF` (the DMA'd copy). Only low-RAM scratch and unrelated
  animated-tile/VRAM buffers differ, from the changed CPU phase.
- Per-call bitmap hashes (`FLASHBACK_PAINT_DUMP=1`, FNV-1a over
  `$7E:2000..$8FFF`) match on every call, not just the last one. Level-1
  room 1: `AA7DDF1F` (count 153); room 1B: `9B617CDD` (254); room 1C:
  `398C6BE3` (146).
- Wall-clock cost of a painter call (the interpreted call blocks the host
  frame loop, so this is what the player feels): 653 ms for 153 records and
  1046 ms for 254 records delegated, vs 0.7 ms / 1.0 ms native. The
  level-start + room-change script runs 95.0 s delegated vs 93.2 s native
  (same 5494 host frames): **~1.8 s less pause**.
- `SNESRECOMP_PRESENT_LOG` black-run counts barely move (native 33/11 vs
  delegated 34/12 with fast-load off/on) because the blocked call presents
  no frames; wall time, not black-frame count, is the right metric.
- `*.apu.tsv` still differs: speculative neighbour frames fill the ring.

### Instrumentation

`FLASHBACK_PAINT_DUMP=1` (off by default) now does two things:

- `src/paint_native.c` logs one `[paint-hash]` line per painter call, from
  both the native and delegated paths: frame, stream/table pointers, record
  count, FNV-1a of the bitmap before and after, guest cycles charged, and
  host ms. This is the differential check to use if the painter is ever
  changed.
- `src/research_watch.c` (uncommitted) still dumps the call args, stream and
  table bytes at `$C219A7`, clip state at `$C21D5F` and the per-row pointers
  at `$C21DEF`.

The out-of-bounds store probe used during the analysis is currently defined
but not registered. `FLASHBACK_WATCH` is unchanged. Keep the dump modes for
future room-format work; they cost nothing when unset.

## Done this session (commits 1262789, 254047b, 8844294)

- Cutscenes: no HD/native flicker (forced-blank frames, two-frame fallback),
  colours taken from the screen, shapes batched until the bitmap clear
  (`$C1625A`), painted backgrounds (wake-up jungle) shown as the original
  pixels under the HD shapes. 0 one-frame flips over intro + wake-up.
- Rooms: neighbour capture faster (time budget, both sides first, then one
  further), early start while a room loads, cache on disk
  (`build/saves/rooms/`), death rooms kept black (e.g. 19 left of 1A).
- Sprites past the field edges drawn in the side areas (BG priority kept).
- Seam blend at joins whose art does not continue.
- Dev aids: `FLASHBACK_ROOM_LABELS=1`, `FLASHBACK_ROOM_LOG=1|file`,
  `FLASHBACK_WIDE_DUMP=dir`, `FLASHBACK_FAST_LOAD=2`.

## Room-change pause (`src/fast_load.c` + native painter)

- A room change = 1 frame cut to black, a long black stretch, 15-frame
  fade-in. Most of the felt pause was one blocking interpreted call: the
  painter at `$C21AD3` took 1.05 s of host time for a 254-record room, and
  0.65 s for the 153-record level-start room. `src/paint_native.c` now does
  those in ~1 ms, so a level-start + room-change script is ~1.8 s shorter.
- Black frames in `SNESRECOMP_PRESENT_LOG` are not a good measure here: the
  blocked call presents nothing, so the count only moves by ~1 frame while
  the wall-clock pause drops by seconds. Time the script or use
  `FLASHBACK_PAINT_DUMP=1` (it reports `cycles` and `ms` per call).
- Remaining load work (interpreted) is the other ~0.5 s of black plus the
  15-frame fade. Hot spots seen during analysis: `$C2CCF0-$C2CEF0` ~15%
  (`$C2CEAA`) and the rest of the room setup; `$C21D90-$C21DF0` is inside
  the painter and now covered by the HLE. `fast_load.c` still shortens the
  black stretch in *guest frames* while it is blanked; it does not remove
  host interpreter work.
- The analysis notes above said the painter was ~51% of interpreted opcodes
  and expected "a few black frames". The opcode share was measured before
  the call-blocking behaviour was understood; the actual win is ~1.7 s of
  host time per session, not a large drop in presented black frames.

## Framework patches (`patches/`, submodule changes stay uncommitted)

- `snesrecomp-interp-fast-bank.patch`: `g_interp_fast_bank`,
  `g_interp_fast_divisor`, `g_interp_fast_all` (cycle charge per bank or
  for every bank; used by cutscene pacing and fast_load).

## Testing tips

- Headless: `SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy`, always set
  `SNESRECOMP_DUMP_DIR`, never run two instances at once.
- `SNESRECOMP_SCREENSHOT_DIR` must be an existing absolute path (the game
  changes into `build/`); `SNESRECOMP_PRESENT_LOG=file.csv` gives a hash and
  luma per presented frame (flicker scans).
- Level 1 script: `tools/research/to_level1.txt`; place Conrad with
  `poke C740 B000` (x = 176) in a script.
- Byte-identical check: dump at fixed frames with the new option off and
  compare all dump files except `*.apu.tsv` (a ring that speculative frames
  also fill).
