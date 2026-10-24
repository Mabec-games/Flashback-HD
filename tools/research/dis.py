# Linear 65816 disassembly of HiROM ranges with REP/SEP width tracking.
# usage: dis.py <framework_root> <rom> <start_pc24_hex> <end_pc24_hex> [m] [x]
import sys
sys.path.insert(0, sys.argv[1] + "/recompiler")
import snes65816 as s
rom = open(sys.argv[2], "rb").read()
if len(rom) % 1024 == 512: rom = rom[512:]
start, end = int(sys.argv[3], 16), int(sys.argv[4], 16)
m = int(sys.argv[5]) if len(sys.argv) > 5 else 1
x = int(sys.argv[6]) if len(sys.argv) > 6 else 1
pc = start
while pc < end:
    bank, addr = pc >> 16, pc & 0xFFFF
    off = ((bank & 0x3F) << 16) | addr
    ins = s.decode_insn(rom, off, addr, bank, m, x)
    if ins is None:
        print(f"${pc:06X}  db ${rom[off]:02X}"); pc += 1; continue
    raw = " ".join(f"{b:02X}" for b in rom[off:off + ins.length])
    print(f"${pc:06X}  {raw:<12} {ins.mnem:<4} {ins.mode:<10} ${ins.operand:X}   m{m}x{x}")
    if ins.mnem == "REP":
        if ins.operand & 0x20: m = 0
        if ins.operand & 0x10: x = 0
    elif ins.mnem == "SEP":
        if ins.operand & 0x20: m = 1
        if ins.operand & 0x10: x = 1
    pc += ins.length
