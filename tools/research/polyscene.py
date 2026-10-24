"""Rebuild a whole cutscene frame from the polygon + VRAM DMA capture.

usage: python3 -I polyscene.py <poly.txt> <dumpdir> <tag> <scale> <out.png>

The cutscene engine rasterizes into one bitmap ($7E:2000) and DMAs the whole
bitmap to VRAM after each batch of shapes:
  - to $5000 (BG2, palette 2): the static scene background, drawn once
  - to $1000 (BG1, palette 1): the animated foreground, redrawn every
    keyframe from a cleared bitmap; colour 0 is transparent
So the shapes drawn since the previous upload belong to the layer the next
upload targets. This redraws both layers for the frame in the <tag> dump,
composites them, and writes the game's frame beside the redraw at <scale>x.
"""

import json
import os
import struct
import sys

from PIL import Image, ImageChops, ImageDraw

VIEW_W, VIEW_H = 240, 128
BG1_VRAM, BG2_VRAM = 0x1000, 0x5000


def read_log(path):
    shapes, uploads = [], []
    for line in open(path):
        p = line.split()
        if len(p) >= 5 and p[1] == "dma":
            if p[2].upper() == "7E2000":
                uploads.append((int(p[0]), int(p[3], 16)))
        elif len(p) >= 7 and p[1] == "ell":
            cx, cy, rx, ry = map(int, p[3:7])
            shapes.append((int(p[0]), int(p[2]) // 4, ("ellipse", cx, cy, rx, ry)))
        elif len(p) >= 3 and not p[-1].startswith("!"):
            n = int(p[2])
            xy = list(map(int, p[3:3 + 2 * n]))
            shapes.append((int(p[0]), int(p[1]) // 4, list(zip(xy[0::2], xy[1::2]))))
    return shapes, uploads


def batch_for(layer_vram, frame, shapes, uploads):
    """Shapes uploaded to layer_vram by the last such upload before frame."""
    hit = None
    for i, (f, dest) in enumerate(uploads):
        if f < frame and dest == layer_vram:
            hit = i
    if hit is None:
        return []
    end = uploads[hit][0]
    start = uploads[hit - 1][0] if hit > 0 else -1
    # shapes logged in the upload's own frame were drawn after its NMI DMA
    return [s for s in shapes if start <= s[0] < end]


def layer_palette(cgram, group, brightness=15):
    out = []
    for i in range(16):
        c = struct.unpack_from("<H", cgram, (group * 16 + i) * 2)[0]
        r, g, b = c & 31, (c >> 5) & 31, (c >> 10) & 31
        out.append(tuple(((v << 3) | (v >> 2)) * brightness // 15 for v in (r, g, b)))
    return out


def raster(batch, scale):
    """Indexed (mode 'L') image of one layer; value = colour index."""
    img = Image.new("L", (VIEW_W * scale, VIEW_H * scale), 0)
    pen = ImageDraw.Draw(img)
    half = scale / 2.0
    for _, index, verts in batch:
        if verts and verts[0] == "ellipse":
            _, cx, cy, rx, ry = verts
            pen.ellipse([(cx - rx) * scale, (cy - ry) * scale,
                         (cx + rx + 1) * scale - 1, (cy + ry + 1) * scale - 1], fill=index)
            continue
        pts = [(x * scale + half, y * scale + half) for x, y in verts]
        if len(pts) == 1:
            x, y = pts[0]
            pen.rectangle([x - half, y - half, x + half - 1, y + half - 1], fill=index)
        elif len(pts) == 2:
            pen.line(pts, fill=index, width=scale)
        else:
            pen.polygon(pts, fill=index)
    return img


def colourize(indexed, colours):
    lut = Image.new("P", (1, 1))
    lut.putpalette([c for rgb in colours for c in rgb] + [0] * (768 - 48))
    img = indexed.copy().convert("P")
    img.putpalette(lut.getpalette())
    return img.convert("RGB")


def compose(shapes, uploads, frame, cgram, backdrop, scale, brightness=15):
    bg2 = raster(batch_for(BG2_VRAM, frame, shapes, uploads), scale)
    bg1 = raster(batch_for(BG1_VRAM, frame, shapes, uploads), scale)
    out = Image.new("RGB", bg1.size, backdrop)
    out.paste(colourize(bg2, layer_palette(cgram, 2, brightness)), (0, 0),
              bg2.point(lambda v: 255 if v else 0))
    out.paste(colourize(bg1, layer_palette(cgram, 1, brightness)), (0, 0),
              bg1.point(lambda v: 255 if v else 0))
    return out


def main():
    poly, dumpdir, tag, scale, out = sys.argv[1:6]
    scale = int(scale)
    info = json.load(open(os.path.join(dumpdir, tag + ".info.json")))
    regs = json.load(open(os.path.join(dumpdir, tag + ".regs.json")))
    cgram = open(os.path.join(dumpdir, tag + ".cgram.bin"), "rb").read()
    fb = open(os.path.join(dumpdir, tag + ".fb.bgrx"), "rb").read()
    game = Image.frombytes("RGBX", (info["fb_width"], info["fb_height"]), fb,
                           "raw", "BGRX").convert("RGB")
    # BG line y shows scroll row y + vscroll + 1, so the viewport sits a line higher.
    ox, oy = (-regs["hscroll"][0]) & 0x3FF, ((-regs["vscroll"][0]) & 0x3FF) - 1
    game = game.crop((ox, oy, ox + VIEW_W, oy + VIEW_H))
    brightness = regs["inidisp"] & 15
    backdrop = layer_palette(cgram, 0, brightness)[0]

    shapes, uploads = read_log(poly)
    frame = info["frame"]
    one = compose(shapes, uploads, frame, cgram, backdrop, 1, brightness)
    diff = ImageChops.difference(one, game).convert("L")
    bad = sum(diff.histogram()[1:])
    print(f"frame {frame}: 1x redraw differs from the game in {bad} of "
          f"{VIEW_W * VIEW_H} pixels ({100.0 * bad / (VIEW_W * VIEW_H):.2f}%)")

    # HD output keeps the game's brightness so the halves compare fairly.
    hd = compose(shapes, uploads, frame, cgram, backdrop, scale, brightness)
    sheet = Image.new("RGB", (hd.width * 2 + 8, hd.height), (40, 40, 40))
    sheet.paste(game.resize(hd.size, Image.NEAREST), (0, 0))
    sheet.paste(hd, (hd.width + 8, 0))
    sheet.save(out)


if __name__ == "__main__":
    main()
