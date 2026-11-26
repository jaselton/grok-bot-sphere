#!/usr/bin/env python3
"""Peak-based wake-word speaker test for the Sphere (after the ESP-Mosaico method).
Puts the Sphere in WAKETEST mode (never fires), plays each phrase through the Mac's speaker with several voices and
rates, and records the PEAK 5-score mean per utterance (logged by the firmware when the event ends). Then suggests a
cutoff: highest negative peak vs. lowest positive peak.
Usage: wake_speaker_test.py "hey charlie" [--port /dev/cu.usbmodemXXXX] [--reps 2]
"""
import argparse, json, subprocess, sys, time, serial

NEG = {
  "hey charlie": ["hey harley", "hey marley", "hey barley", "hey Charles", "hey Charlotte", "hey Carly", "hey chili",
                  "hey Charlene", "hey sharpie", "okay Charlie Brown", "Charlie", "hey Siri", "hey Google", "hey honey",
                  "hey darling", "hey Molly", "hey Arlie", "hey grok", "what's the weather", "hey, good morning"],
  "hey grok": ["hey greg", "great job", "hey google", "hey rock", "hey Brock", "hey, good morning", "hey crook", "okay"],
}
POS = {"hey charlie": ["hey Charlie", "hey, Charlie", "Hey Charlie!"], "hey grok": ["hey Grok", "hey, Grok"]}
VOICES = ["Samantha", "Alex", "Fred", "Victoria", "Daniel", "Karen", "Tom"]

def voices_available():
    out = subprocess.run(["say", "-v", "?"], capture_output=True, text=True).stdout
    have = {l.split()[0] for l in out.splitlines() if l.strip()}
    return [v for v in VOICES if v in have] or [None]

def main():
    ap = argparse.ArgumentParser(); ap.add_argument("word"); ap.add_argument("--port", required=True, help="the Sphere's /dev/cu.usbmodem… port")
    ap.add_argument("--reps", type=int, default=1); a = ap.parse_args()
    s = serial.Serial(a.port, 115200, timeout=0.1)
    s.write(b"WAKETEST ON\n"); time.sleep(0.5); s.reset_input_buffer()
    results = []
    def play(text, voice, rate):
        s.reset_input_buffer()
        cmd = ["say", "-r", str(rate)] + (["-v", voice] if voice else []) + [text]
        subprocess.run(cmd); t = time.time(); peaks = []; buf = b""
        while time.time() - t < 1.8:
            buf += s.read(4096)
            *lines, buf = buf.split(b"\n")
            for l in lines:
                if b"[wake] event peak" in l: peaks.append(float(l.split()[-1]))
        return max(peaks) if peaks else 0.0
    vs = voices_available()
    for kind, phrases in (("pos", POS[a.word]), ("neg", NEG[a.word])):
        for ph in phrases:
            for v in vs:
                for rate in (150, 185):
                    for _ in range(a.reps):
                        p = play(ph, v, rate); results.append((kind, ph, v, rate, p))
                        print(f"{kind} {p:.3f}  {ph!r:28} {v} {rate}", flush=True)
    s.write(b"WAKETEST OFF\n")
    pos = sorted(r[4] for r in results if r[0] == "pos"); neg = sorted(r[4] for r in results if r[0] == "neg")
    worst_neg = sorted([r for r in results if r[0] == "neg"], key=lambda r: -r[4])[:5]
    print("\npositives: n=%d min %.3f median %.3f" % (len(pos), pos[0], pos[len(pos)//2]))
    print("negatives: n=%d max %.3f" % (len(neg), neg[-1]))
    print("top negatives:", [(r[1], r[2], round(r[4], 3)) for r in worst_neg])
    for c in (0.6, 0.7, 0.8, 0.85, 0.9, 0.95):
        print(f"cutoff {c:.2f}: misses {sum(p < c for p in pos)}/{len(pos)}, false wakes {sum(n >= c for n in neg)}/{len(neg)}")
    import os
    out = f"wake_test_{a.word.replace(' ', '_')}.json"
    json.dump(results, open(out, "w")); print("saved", out)

if __name__ == "__main__": main()
