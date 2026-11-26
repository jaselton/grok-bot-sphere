#!/usr/bin/env python3
"""Drive the sphere over USB and save what each face really draws: face_shots.py PORT OUTDIR"""
import sys, time, serial
from PIL import Image
port, out = sys.argv[1], sys.argv[2]
s = serial.Serial(port, 115200, timeout=0.2); time.sleep(0.5); s.reset_input_buffer()
def cmd(c, wait=1.5):
    s.write((c + "\n").encode()); t = time.time(); buf = b""
    while time.time() - t < wait: buf += s.read(4096)
    return buf
def shot(name):
    s.reset_input_buffer(); s.write(b"SHOT\n"); buf = b""; t = time.time()
    while b"SHOT END" not in buf and time.time() - t < 15: buf += s.read(65536)
    i = buf.index(b"SHOT 360 360\n") + len(b"SHOT 360 360\n"); raw = buf[i:i + 360 * 360 * 2]
    im = Image.new("RGB", (360, 360)); px = im.load()
    for k in range(360 * 360):
        v = raw[2 * k] | raw[2 * k + 1] << 8
        px[k % 360, k // 360] = ((v >> 11) << 3, ((v >> 5) & 63) << 2, (v & 31) << 3)
    im.save(f"{out}/{name}.png"); print("saved", name)
log = cmd("TALKLEFT 18.5", 0.5)
for n, c in [("dive", "FACE 1"), ("timer_idle", "FACE 2"), ("fuel", "FACE 3"), ("stinger", "FACE 4")]:
    log += cmd(c); shot(n)
log += cmd("FACE 3", 0.5); log += cmd("TALKLEFT 3.2"); shot("fuel_low")
log += cmd("TIMER 750", 2.5); shot("timer_run")
log += cmd("RING", 1.0); shot("caution")
log += cmd("TAP", 1.0); log += cmd("FACE 0", 0.5)
print("\n".join(l for l in log.decode(errors="replace").splitlines() if "[faces]" in l or "rror" in l or "Guru" in l))
