import sys, os, json
d = sys.argv[1]; n = int(sys.argv[2])
prev = None
for i in range(n):
    t = "c%02d" % i
    v = open(os.path.join(d, t + ".vram.bin"), "rb").read()
    fb = open(os.path.join(d, t + ".fb.bgrx"), "rb").read()
    r = json.load(open(os.path.join(d, t + ".regs.json")))
    info = json.load(open(os.path.join(d, t + ".info.json")))
    if prev:
        pv, pfb = prev
        blocks = sorted({(j // 2) >> 10 for j in range(0, 65536, 2) if v[j:j+2] != pv[j:j+2]})
        nch = sum(1 for j in range(0, 65536, 2) if v[j:j+2] != pv[j:j+2])
        fbch = fb != pfb
        print(info["frame"], "vram words changed", nch, "blocks", [hex(b << 10) for b in blocks], "fb changed", fbch, "tm", r["tm"], "bgsc", r["bgsc"], "tile", hex(r["bg_tile_adr"]), "hs", r["hscroll"][:2])
    prev = (v, fb)
