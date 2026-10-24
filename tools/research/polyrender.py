"""Redraw captured cutscene polygons and compare them with the game's frame.

usage: python3 -I polyrender.py <poly.txt> <dumpdir> <tag> <frame_lo> <frame_hi>
                                <scale> <out.png> [--clear]

Draws every shape logged by FLASHBACK_POLY_LOG with frame_lo <= frame <= frame_hi
into a 240x128 canvas at <scale>x, using the palette from the <tag> state dump.
The output is the game's own frame (cropped to the cutscene viewport and
nearest-neighbour scaled) beside the redraw, plus a 1x pixel diff count.
Without --clear the canvas starts from the dump's 1x frame, so shapes drawn in
an earlier window are kept, as the game's bitmap keeps them.
"""

import json
import os
import struct
import sys

from PIL import Image, ImageDraw

VIEW_W, VIEW_H = 240, 128


def load_dump(dumpdir, tag):
    regs = json.load(open(os.path.join(dumpdir, tag + ".regs.json")))
    info = json.load(open(os.path.join(dumpdir, tag + ".info.json")))
    vram = open(os.path.join(dumpdir, tag + ".vram.bin"), "rb").read()
    cgram = open(os.path.join(dumpdir, tag + ".cgram.bin"), "rb").read()
    fb = open(os.path.join(dumpdir, tag + ".fb.bgrx"), "rb").read()
    w, h = info["fb_width"], info["fb_height"]
    frame = Image.frombytes("RGBX", (w, h), fb, "raw", "BGRX").convert("RGB")
    return regs, vram, cgram, frame


def palette(cgram, vram, regs):
    base = (regs["bgsc"][0] >> 2) << 10
    group = (struct.unpack_from("<H", vram, base * 2)[0] >> 10) & 7
    colours = []
    for i in range(16):
        c = struct.unpack_from("<H", cgram, (group * 16 + i) * 2)[0]
        r, g, b = c & 31, (c >> 5) & 31, (c >> 10) & 31
        colours.append(((r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2)))
    return colours


def viewport_origin(regs):
    # The cutscene layers are scrolled so viewport (0,0) lands on screen.
    return (-regs["hscroll"][0]) & 0x3FF, (-regs["vscroll"][0]) & 0x3FF


def load_shapes(path, lo, hi):
    shapes = []
    for line in open(path):
        parts = line.split()
        if len(parts) < 3 or parts[-1].startswith("!"):
            continue
        frame, colour, count = int(parts[0]), int(parts[1]), int(parts[2])
        if lo <= frame <= hi:
            xy = list(map(int, parts[3:3 + count * 2]))
            shapes.append((colour // 4, list(zip(xy[0::2], xy[1::2]))))
    return shapes


def draw(shapes, colours, scale, background):
    img = background.resize((VIEW_W * scale, VIEW_H * scale), Image.NEAREST)
    pen = ImageDraw.Draw(img)
    half = scale / 2.0
    for index, verts in shapes:
        pts = [(x * scale + half, y * scale + half) for x, y in verts]
        if len(pts) == 1:
            pen.rectangle([pts[0][0] - half, pts[0][1] - half,
                           pts[0][0] + half, pts[0][1] + half], fill=colours[index])
        elif len(pts) == 2:
            pen.line(pts, fill=colours[index], width=max(1, scale))
        else:
            pen.polygon(pts, fill=colours[index])
    return img


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    clear = "--clear" in sys.argv
    poly, dumpdir, tag, lo, hi, scale, out = args
    lo, hi, scale = int(lo), int(hi), int(scale)
    regs, vram, cgram, frame = load_dump(dumpdir, tag)
    colours = palette(cgram, vram, regs)
    ox, oy = viewport_origin(regs)
    game = frame.crop((ox, oy, ox + VIEW_W, oy + VIEW_H))
    start = Image.new("RGB", (VIEW_W, VIEW_H), colours[0]) if clear else game
    shapes = load_shapes(poly, lo, hi)

    one = draw(shapes, colours, 1, start)
    diff = sum(1 for a, b in zip(one.getdata(), game.getdata()) if a != b)
    print(f"{len(shapes)} shapes, viewport origin ({ox},{oy}), "
          f"1x diff vs game: {diff} of {VIEW_W * VIEW_H} pixels")

    hd = draw(shapes, colours, scale, start)
    sheet = Image.new("RGB", (VIEW_W * scale * 2 + 8, VIEW_H * scale), (40, 40, 40))
    sheet.paste(game.resize(hd.size, Image.NEAREST), (0, 0))
    sheet.paste(hd, (VIEW_W * scale + 8, 0))
    sheet.save(out)


if __name__ == "__main__":
    main()
