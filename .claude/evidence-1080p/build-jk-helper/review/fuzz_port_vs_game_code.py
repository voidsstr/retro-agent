import random, subprocess, sys
sys.path.insert(0, ".")
import emu_modelist as E
random.seed(20260929)
W = [320, 400, 512, 640, 720, 800, 848, 1024, 1152, 1280, 1360, 1400, 1440, 1600, 1680, 1776, 1920]
H = [200, 240, 300, 384, 480, 576, 600, 720, 768, 800, 864, 900, 960, 1000, 1024, 1050, 1080]
BPP = [(8, 0x60), (16, 0x40), (32, 0x40), (24, 0x40)]
bad = 0
for trial in range(400):
    game = random.choice(["df2", "mots"])
    n1 = random.randint(0, 8); n2 = random.randint(0, 80)
    def mode(force320=False):
        w = 320 if force320 else random.choice(W)
        h = random.choice(H); b, pf = random.choice(BPP)
        return (w, h, pf, b)
    p1 = [mode(True) for _ in range(n1)]
    p2 = [mode(random.random() < 0.15) for _ in range(n2)]
    if random.random() < 0.3:          # duplicates
        p2 += random.sample(p2, min(len(p2), random.randint(0, 6)))
    pm = random.choice(["natural", "aligned32"])
    n, emu = E.run(game, p1, p2, pm)
    lines = ["filter %d" % (1 if game == "mots" else 0)]
    for pas, seq in ((1, p1), (2, p2)):
        for (w, h, pf, b) in seq:
            pitch = w * b // 8
            if pm == "aligned32": pitch = (pitch + 31) & ~31
            lines.append("%d %d %d %d %x %d" % (pas, w, h, pitch, pf, b))
    out = subprocess.run(["./port"], input="\n".join(lines) + "\n", capture_output=True, text=True).stdout.split("\n")
    port = []
    for l in out:
        parts = l.split()
        if len(parts) >= 3 and parts[0].isdigit():
            mx = parts[1] == "X"
            wh = parts[2] if mx else parts[1]
            bp = parts[3] if mx else parts[2]
            w, h = wh.split("x"); port.append((1 if mx else 0, int(w), int(h), int(bp[:-3])))
    if port != [tuple(x) for x in emu]:
        bad += 1
        if bad < 4: print("MISMATCH trial", trial, game, len(p1), len(p2), pm)
print("trials 400, mismatches", bad)
