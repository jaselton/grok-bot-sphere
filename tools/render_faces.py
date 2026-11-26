#!/usr/bin/env python3
"""Render the Sphere's extra faces (360x360): UH-1-style instrument faces and a '67 big-block stinger clock.

  huey_timer    UH-1 panel-instrument countdown timer (60-min scale, live sweep + digits)
  huey_fuel     UH-1 fuel-quantity-style gauge for Grok talk minutes left today
  huey_caution  UH-1 caution-panel alarm screen (MASTER CAUTION = tap to reset)
  stinger       '67 427 stinger-hood clock (yellow paint, black stinger, chrome bezel)

No aircraft maker, carmaker or watch brand names or logos. Each face writes <name>.png (static parts only) and
<name>_preview.png (with the live parts drawn the way the firmware will draw them).
Usage: render_faces.py [--out DIR]
"""
import argparse, math, os
from PIL import Image, ImageDraw
from render_watch_dial import S, N, C, font, pol, text_center, quad, ring, DIN, AVENIR, write_header

FS = "/System/Library/Fonts/Supplemental"
FUTURA = f"{FS}/Futura.ttc"          # index 0 Medium, 2 Bold, 4 Condensed ExtraBold
DINC = f"{FS}/DIN Condensed Bold.ttf"

MIL_WHITE = (236, 236, 230)
MIL_BLACK = (14, 14, 15)
CASE = (40, 41, 44)
AMBER = (255, 176, 0)
CAUTION_RED = (220, 30, 24)
YELLOW_BAND = (240, 200, 0)

def finish(img):
    mask = Image.new("L", (N, N), 0); ImageDraw.Draw(mask).ellipse([0, 0, N - 1, N - 1], fill=255)
    out = Image.new("RGBA", (N, N), (0, 0, 0, 255)); out.paste(img, (0, 0), mask)
    return out.resize((360, 360), Image.LANCZOS).convert("RGB")

def up(im):  # 1x preview -> supersampled canvas for drawing live parts
    return im.convert("RGBA").resize((N, N), Image.BICUBIC)

def screw(d, deg, r=166):
    x, y = pol(r, deg); rr = 7 * S
    d.ellipse([x - rr, y - rr, x + rr, y + rr], fill=(70, 71, 75), outline=(20, 20, 22), width=S)
    a = math.radians(deg + 30); dx, dy = math.cos(a) * rr * 0.8, math.sin(a) * rr * 0.8
    d.line([x - dx, y - dy, x + dx, y + dy], fill=(18, 18, 20), width=int(1.8 * S))

def instrument_case(d):
    """Round panel instrument: dark case ring, 4 slotted screws, thin inner glass ring."""
    ring(d, 180, 0, CASE)
    for deg in (45, 135, 225, 315): screw(d, deg)
    d.ellipse([C - 156 * S, C - 156 * S, C + 156 * S, C + 156 * S], fill=(4, 4, 5))
    d.ellipse([C - 154 * S, C - 154 * S, C + 154 * S, C + 154 * S], fill=MIL_BLACK)

def needle(d, deg, length, tail=26, w=7, col=MIL_WHITE, cx=C, cy=C):
    a = math.radians(deg); ux, uy = math.sin(a), -math.cos(a); px, py = -uy, ux
    L, T, W = length * S, tail * S, w * S / 2
    pts = [(cx - ux * T + px * W, cy - uy * T + py * W), (cx + ux * L * 0.86 + px * W * 0.7, cy + uy * L * 0.86 + py * W * 0.7),
           (cx + ux * L, cy + uy * L), (cx + ux * L * 0.86 - px * W * 0.7, cy + uy * L * 0.86 - py * W * 0.7),
           (cx - ux * T - px * W, cy - uy * T - py * W)]
    d.polygon(pts, fill=col)
    rr = 12 * S; d.ellipse([cx - rr, cy - rr, cx + rr, cy + rr], fill=(30, 30, 32), outline=(90, 90, 94), width=S)
    rr = 3 * S; d.ellipse([cx - rr, cy - rr, cx + rr, cy + rr], fill=(120, 120, 124))

def arc_band(d, r_out, r_in, a0, a1, fill):
    """Band between radii from a0 to a1 (deg clockwise from 12)."""
    steps = max(8, int(abs(a1 - a0)))
    pts = [pol(r_out, a0 + (a1 - a0) * i / steps) for i in range(steps + 1)]
    pts += [pol(r_in, a1 - (a1 - a0) * i / steps) for i in range(steps + 1)]
    d.polygon(pts, fill=fill)

# ---------------------------------------------------------------- UH-1 countdown timer
def huey_timer():
    img = Image.new("RGBA", (N, N), (0, 0, 0, 255)); d = ImageDraw.Draw(img)
    instrument_case(d)
    for t in range(60):                                       # 60-minute countdown scale
        deg = t * 6
        if t % 5 == 0: quad(d, 128, 150, deg, 4.2, MIL_WHITE)
        else: quad(d, 140, 150, deg, 1.8, MIL_WHITE)
    for t in range(0, 60, 5):
        text_center(d, pol(112, t * 6), str(t), font(FUTURA, 21, 2), MIL_WHITE)
    text_center(d, (C, C - 40 * S), "TIME TO GO", font(FUTURA, 11, 2), MIL_WHITE)
    # digit window (live digits)
    d.rounded_rectangle([C - 56 * S, C + 22 * S, C + 56 * S, C + 70 * S], radius=6 * S, fill=(0, 0, 0), outline=(90, 90, 94), width=int(1.5 * S))
    text_center(d, (C, C + 80 * S), "MIN          SEC", font(FUTURA, 8, 2), (170, 170, 166))
    text_center(d, (C, C - 56 * S), "UH-1H", font(FUTURA, 8, 2), (150, 150, 146))
    return finish(img)

def huey_timer_preview(face, mm=12, ss=30):
    im = up(face); d = ImageDraw.Draw(im)
    rem = mm + ss / 60
    arc_band(d, 152, 152 - 3, 0, rem * 6, AMBER)            # remaining sweep on the rim
    text_center(d, (C, C + 46 * S), f"{mm:02d}:{ss:02d}", font(DINC, 44), AMBER)
    needle(d, rem * 6, 146, tail=20, w=6)
    return im.resize((360, 360), Image.LANCZOS).convert("RGB")

# ---------------------------------------------------------------- UH-1 fuel-style talk gauge
FUEL_A0, FUEL_A1, FUEL_MAX = -135, 135, 30

def fuel_deg(v): return FUEL_A0 + (FUEL_A1 - FUEL_A0) * v / FUEL_MAX

def huey_fuel():
    img = Image.new("RGBA", (N, N), (0, 0, 0, 255)); d = ImageDraw.Draw(img)
    instrument_case(d)
    arc_band(d, 150, 140, fuel_deg(0), fuel_deg(5), YELLOW_BAND)  # low caution band
    quad(d, 126, 150, fuel_deg(0), 5, CAUTION_RED)
    for v in range(0, FUEL_MAX + 1):
        deg = fuel_deg(v)
        if v == 0: continue
        if v % 5 == 0: quad(d, 126, 150, deg, 4.2, MIL_WHITE)
        else: quad(d, 138, 150, deg, 1.8, MIL_WHITE)
    for v in range(0, FUEL_MAX + 1, 5):
        text_center(d, pol(108, fuel_deg(v)), str(v), font(FUTURA, 21, 2), MIL_WHITE)
    text_center(d, (C, C - 64 * S), "TALK QTY", font(FUTURA, 12, 2), MIL_WHITE)
    text_center(d, (C, C - 48 * S), "MINUTES", font(FUTURA, 8.5, 2), (170, 170, 166))
    # bottom: digital readout window + dim caution capsule
    d.rounded_rectangle([C - 34 * S, C + 44 * S, C + 34 * S, C + 72 * S], radius=4 * S, fill=(0, 0, 0), outline=(90, 90, 94), width=int(1.5 * S))
    d.rounded_rectangle([C - 44 * S, C + 84 * S, C + 44 * S, C + 102 * S], radius=3 * S, fill=(36, 30, 10), outline=(70, 60, 20), width=S)
    text_center(d, (C, C + 93 * S), "5 MIN TALK", font(FUTURA, 9, 2), (90, 76, 30))
    text_center(d, (C, C + 118 * S), "TODAY", font(FUTURA, 7.5, 2), (150, 150, 146))
    return finish(img)

def huey_fuel_preview(face, left=18.5, low=False):
    im = up(face); d = ImageDraw.Draw(im)
    text_center(d, (C, C + 58 * S), f"{left:4.1f}", font(DINC, 24), MIL_WHITE)
    if low:
        d.rounded_rectangle([C - 44 * S, C + 84 * S, C + 44 * S, C + 102 * S], radius=3 * S, fill=AMBER)
        text_center(d, (C, C + 93 * S), "5 MIN TALK", font(FUTURA, 9, 2), (20, 16, 0))
    needle(d, fuel_deg(left), 146)
    return im.resize((360, 360), Image.LANCZOS).convert("RGB")

# ---------------------------------------------------------------- UH-1 caution-panel alarm
TILES = [["ENG OIL PRESS", "XMSN OIL HOT", "FUEL BOOST"],
         ["CHIP DETR", "WAKE UP", "INST INVERTER"],
         ["HYD PRESS", "ENG ICE DET", "DC GEN"]]

def huey_caution():
    img = Image.new("RGBA", (N, N), (0, 0, 0, 255)); d = ImageDraw.Draw(img)
    ring(d, 180, 0, CASE)
    d.ellipse([C - 168 * S, C - 168 * S, C + 168 * S, C + 168 * S], fill=(22, 22, 24))
    # caution panel (3x3 legend tiles; the lit one is drawn live)
    tw, th, gx, gy = 80, 28, 6, 6
    x0 = C - (3 * tw + 2 * gx) * S / 2; y0 = C - 138 * S
    for r, row in enumerate(TILES):
        for c, lab in enumerate(row):
            x = x0 + c * (tw + gx) * S; y = y0 + r * (th + gy) * S
            d.rectangle([x, y, x + tw * S, y + th * S], fill=(10, 10, 11), outline=(60, 60, 64), width=S)
            text_center(d, (x + tw * S / 2, y + th * S / 2), lab, font(FUTURA, 7.5, 2), (84, 74, 38))
    # MASTER CAUTION press-to-reset light (drawn unlit; firmware lights it)
    d.rounded_rectangle([C - 70 * S, C - 22 * S, C + 70 * S, C + 30 * S], radius=6 * S, fill=(40, 30, 6), outline=(90, 90, 94), width=int(2 * S))
    text_center(d, (C, C - 4 * S), "MASTER", font(FUTURA, 15, 2), (100, 76, 20))
    text_center(d, (C, C + 15 * S), "CAUTION", font(FUTURA, 15, 2), (100, 76, 20))
    text_center(d, (C, C + 42 * S), "PRESS TO RESET", font(FUTURA, 7.5, 2), (150, 150, 146))
    return finish(img)

def huey_caution_preview(face, when="06:30", lit=True):
    im = up(face); d = ImageDraw.Draw(im)
    tw, th, gx, gy = 80, 28, 6, 6
    x0 = C - (3 * tw + 2 * gx) * S / 2; y0 = C - 138 * S
    x = x0 + 1 * (tw + gx) * S; y = y0 + 1 * (th + gy) * S
    d.rectangle([x, y, x + tw * S, y + th * S], fill=AMBER)
    text_center(d, (x + tw * S / 2, y + th * S / 2), "WAKE UP", font(FUTURA, 9, 2), (20, 16, 0))
    if lit:
        d.rounded_rectangle([C - 70 * S, C - 22 * S, C + 70 * S, C + 30 * S], radius=6 * S, fill=AMBER, outline=(90, 90, 94), width=int(2 * S))
        text_center(d, (C, C - 4 * S), "MASTER", font(FUTURA, 15, 2), (20, 16, 0))
        text_center(d, (C, C + 15 * S), "CAUTION", font(FUTURA, 15, 2), (20, 16, 0))
    text_center(d, (C, C + 80 * S), when, font(DINC, 40), MIL_WHITE)
    text_center(d, (C, C + 108 * S), "ALARM", font(FUTURA, 8.5, 2), (170, 170, 166))
    return im.resize((360, 360), Image.LANCZOS).convert("RGB")

# ---------------------------------------------------------------- '67 427 stinger clock
PAINT = (236, 200, 52)      # yellow like the reference photo; change to match the real car
STINGER = (12, 12, 13)

def stinger_shape(d, grow=0.0, fill=STINGER):
    """The hood stinger: a scoop at the top, a short shoulder, then a long taper to a spear point below center.
    grow > 0 makes an outline that much bigger (px) all round."""
    def P(x, y): return (C + x * S, C + y * S)
    g = grow
    pts = [P(-40 - g, -200), P(-40 - g, -98 + g * 0.3), P(-17 - g, -80 + g * 0.2), P(0, 142 + g * 2.6),
           P(17 + g, -80 + g * 0.2), P(40 + g, -98 + g * 0.3), P(40 + g, -200)]
    d.polygon(pts, fill=fill)

def stinger():
    img = Image.new("RGBA", (N, N), (0, 0, 0, 255)); d = ImageDraw.Draw(img)
    for r in range(180, 158, -1):                         # chrome bezel
        t = (180 - r) / 22; v = int(120 + 120 * math.sin(t * math.pi))
        ring(d, r, 0, (v, v, min(255, v + 6)))
    ring(d, 158, 0, PAINT)
    # pinstripe outline, paint gap, black stinger (like the factory hood)
    stinger_shape(d, 6, STINGER); stinger_shape(d, 4, PAINT); stinger_shape(d, 0, STINGER)
    d.ellipse([C - 158 * S, C - 158 * S, C + 158 * S, C + 158 * S], outline=(70, 70, 74), width=int(1.2 * S))
    # minute track, black on paint (skipped where the stinger is)
    for m in range(60):
        if m in (0, 1, 59, 29, 30, 31): continue
        deg = m * 6
        if m % 5 == 0: quad(d, 138, 156, deg, 3.6, (16, 16, 16))
        else: quad(d, 149, 156, deg, 1.4, (40, 36, 20))
    for h in range(1, 12):
        if h == 6: continue
        text_center(d, pol(118, h * 30), str(h), font(FUTURA, 23, 2), (16, 16, 16))
    text_center(d, (C, C - 128 * S), "427", font(FUTURA, 24, 2), PAINT)
    return finish(img)

def stinger_preview(face, hh=10, mm=9, ss=37):
    im = up(face); d = ImageDraw.Draw(im)
    def hand(deg, length, w, col):
        a = math.radians(deg); ux, uy = math.sin(a), -math.cos(a); px, py = -uy, ux
        L, W, T = length * S, w * S / 2, 18 * S
        pts = [(C - ux * T + px * W, C - uy * T + py * W), (C + ux * L + px * W * 0.35, C + uy * L + py * W * 0.35),
               (C + ux * L - px * W * 0.35, C + uy * L - py * W * 0.35), (C - ux * T - px * W, C - uy * T - py * W)]
        d.polygon(pts, fill=col, outline=(30, 30, 32), width=S)
    hand((hh % 12) * 30 + mm * 0.5, 88, 10, (214, 214, 220))
    hand(mm * 6 + ss * 0.1, 132, 8, (214, 214, 220))
    a = math.radians(ss * 6); ux, uy = math.sin(a), -math.cos(a)
    d.line([C - ux * 26 * S, C - uy * 26 * S, C + ux * 146 * S, C + uy * 146 * S], fill=CAUTION_RED, width=int(2.4 * S))
    rr = 9 * S; d.ellipse([C - rr, C - rr, C + rr, C + rr], fill=(210, 210, 214), outline=(90, 90, 94), width=S)
    return im.resize((360, 360), Image.LANCZOS).convert("RGB")

if __name__ == "__main__":
    ap = argparse.ArgumentParser(); ap.add_argument("--out", default="."); a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    faces = {"huey_timer": (huey_timer, huey_timer_preview), "huey_fuel": (huey_fuel, huey_fuel_preview),
             "huey_caution": (huey_caution, huey_caution_preview), "stinger": (stinger, stinger_preview)}
    for name, (mk, pv) in faces.items():
        f = mk(); f.save(os.path.join(a.out, f"{name}.png")); pv(f).save(os.path.join(a.out, f"{name}_preview.png"))
    # contact sheet of all previews incl. the dive watch
    names = ["watch"] + list(faces)
    sheet = Image.new("RGB", (len(names) * 380 + 20, 400), (24, 24, 26))
    for i, n in enumerate(names):
        p = os.path.join(a.out, f"{n}_preview.png")
        if os.path.exists(p): sheet.paste(Image.open(p), (20 + i * 380, 20))
    sheet.save(os.path.join(a.out, "faces_sheet.png"))
    print("wrote", a.out)
