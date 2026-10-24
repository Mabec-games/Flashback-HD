import sys, os, json, struct
d, t = sys.argv[1], sys.argv[2]
r = json.load(open(os.path.join(d, t + ".regs.json")))
print({k: r[k] for k in ("bgmode","bgsc","bg_tile_adr","tm","ts","obsel","hscroll","vscroll","cgwsel","cgadsub","hdmaen_last")})
oam = open(os.path.join(d, t + ".oam.bin"), "rb").read()
hi = oam[512:]
vis = []
for i in range(128):
    x, y, tile, attr = oam[i*4:i*4+4]
    hb = (hi[i//4] >> ((i%4)*2)) & 3
    x |= (hb & 1) << 8
    if y < 224 and x < 256 or x >= 0x1F0 and y < 224:
        vis.append((i, x, y, tile | ((attr & 1) << 8), (attr>>1)&7, "big" if hb & 2 else "small"))
print("visible sprites:", len(vis)); [print("  ", v) for v in vis[:40]]
v = open(os.path.join(d, t + ".vram.bin"), "rb").read()
for bg in range(3):
    base = (r["bgsc"][bg] >> 2) << 10
    tiles = [struct.unpack_from("<H", v, (base + i) * 2)[0] & 0x3FF for i in range(32 * 32)]
    print(f"BG{bg+1} map@{hex(base)} size={r['bgsc'][bg]&3} uniq={len(set(tiles))} row0:", [hex(x) for x in tiles[:10]], "row10:", [hex(x) for x in tiles[320:330]])
