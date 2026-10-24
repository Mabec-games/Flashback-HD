# Research tools (step 1: map the engine)

Run headless with state dumps:

    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy SNESRECOMP_DUMP_DIR=/some/dir \
      ./build/FlashbackSNESRecomp --no-launcher --script tools/research/to_level1.txt <rom>

- `to_level1.txt` – input script that reaches the first Titan room and dumps it.
- `scene.py <dumpdir> <tag>` – PPU mode, layer tilemaps, visible sprites.
- `cad.py <dumpdir> <n>` – per-frame VRAM change cadence over dumps `c00..c<n-1>`.
- `dis.py <framework> <rom> <start> <end> [m] [x]` – linear 65816 disassembly (HiROM).
- `xref.py <rom> <lo> <hi>` – JSL/JSR/JML references into a range (byte scan, expect false positives).
- `wdiff.py <dumpdir> <tags...>` – WRAM change regions between dumps.
- `polyrender.py`, `polyscene.py` – redraw captured cutscene shapes (see CUTSCENE_RENDERER.md).
- `CUTSCENE_RENDERER.md` – findings on the polygon cutscene renderer.

Run the Python with `python3 -I`.
