# Find JSL/JSR/JML references into a target range in a HiROM image.
# usage: xref.py <rom> <lo_pc24_hex> <hi_pc24_hex>
import sys
rom = open(sys.argv[1], "rb").read()
lo, hi = int(sys.argv[2], 16), int(sys.argv[3], 16)
for off in range(len(rom) - 3):
    op = rom[off]
    bank = 0xC0 | (off >> 16)
    if op in (0x22, 0x5C):  # JSL / JML long
        t = rom[off+1] | rom[off+2] << 8 | rom[off+3] << 16
        if lo <= t <= hi:
            print(f"{'JSL' if op == 0x22 else 'JML'} ${t:06X} from ${bank:02X}:{off & 0xFFFF:04X}")
    elif op in (0x20, 0x4C) and (lo >> 16) == bank:  # JSR / JMP abs, same bank
        t = (bank << 16) | rom[off+1] | rom[off+2] << 8
        if lo <= t <= hi:
            print(f"{'JSR' if op == 0x20 else 'JMP'} ${t:06X} from ${bank:02X}:{off & 0xFFFF:04X}")
