import sys, os
d = sys.argv[1]; tags = sys.argv[2:]
for a, b in zip(tags, tags[1:]):
    wa = open(os.path.join(d, a + ".wram.bin"), "rb").read()
    wb = open(os.path.join(d, b + ".wram.bin"), "rb").read()
    ch = [i for i in range(len(wa)) if wa[i] != wb[i]]
    runs = []
    for i in ch:
        if runs and i - runs[-1][1] <= 64: runs[-1][1] = i
        else: runs.append([i, i])
    big = [(hex(s), e - s + 1) for s, e in runs if e - s > 256]
    print(f"{a}->{b}: {len(ch)} bytes changed; large regions: {big}")
