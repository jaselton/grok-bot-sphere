#!/usr/bin/env python3
"""Render the Sphere's dive-chronograph watch dial (360x360) and emit an LVGL RGB565 image header.

Inspired by classic dive chronographs (black dial, orange 15-minute bezel section, bold lume indices, orange 12,
two subdials, date at 6). No watch brand names or logos: the dial says CHARLIE.

Outputs:  watch_dial.png (static dial), watch_preview.png (dial + hands at 10:09:37), watch_dial.h (firmware image)
Usage:    render_watch_dial.py [--brand CHARLIE] [--script "Chief of Staff"] [--line1 "DESK CHRONOMETER"] [--line2 "EST. 2026"] [--out DIR]
"""
import argparse, math, os
from PIL import Image, ImageDraw, ImageFont, ImageFilter

S = 4                      # supersample
N = 360 * S
C = N / 2
F = "/System/Library/Fonts"
FS = "/System/Library/Fonts/Supplemental"
DIN = f"{FS}/DIN Alternate Bold.ttf"
AVENIR = f"{F}/Avenir Next.ttc"
SCRIPT = f"{FS}/SnellRoundhand.ttc"

ORANGE = (255, 94, 0)
WHITE = (242, 242, 240)
LUME = (236, 236, 228)
GREY = (150, 152, 156)
DARK = (8, 8, 9)
CERAMIC = (22, 23, 26)

def font(path, px, index=0):
    return ImageFont.truetype(path, int(px * S), index=index)

def pol(r, deg):           # deg clockwise from 12 o'clock
    a = math.radians(deg)
    return C + r * S * math.sin(a), C - r * S * math.cos(a)

def text_center(d, xy, s, f, fill):
    l, t, r, b = d.textbbox((0, 0), s, font=f)
    d.text((xy[0] - (r - l) / 2 - l, xy[1] - (b - t) / 2 - t), s, font=f, fill=fill)

def rotated_text(img, s, f, fill, r, deg):
    """Bezel numerals: rotated tangentially, readable from outside (top of glyph points outward)."""
    tmp = Image.new("RGBA", (int(f.size * 3), int(f.size * 2)), (0, 0, 0, 0))
    td = ImageDraw.Draw(tmp)
    text_center(td, (tmp.width / 2, tmp.height / 2), s, f, fill)
    tmp = tmp.rotate(-deg, resample=Image.BICUBIC, expand=True)
    x, y = pol(r, deg)
    img.alpha_composite(tmp, (int(x - tmp.width / 2), int(y - tmp.height / 2)))

def quad(d, r0, r1, deg, w, fill, outline=None):
    """Radial bar from r0 to r1 at angle deg, width w (px at 1x)."""
    a = math.radians(deg); ux, uy = math.sin(a), -math.cos(a); px, py = -uy, ux
    hw = w * S / 2
    p0 = (C + r0 * S * ux, C + r0 * S * uy); p1 = (C + r1 * S * ux, C + r1 * S * uy)
    pts = [(p0[0] + px * hw, p0[1] + py * hw), (p1[0] + px * hw, p1[1] + py * hw),
           (p1[0] - px * hw, p1[1] - py * hw), (p0[0] - px * hw, p0[1] - py * hw)]
    d.polygon(pts, fill=fill, outline=outline, width=S if outline else 0)

def ring(d, r_out, r_in, fill):
    d.ellipse([C - r_out * S, C - r_out * S, C + r_out * S, C + r_out * S], fill=fill)
    if r_in > 0:
        d.ellipse([C - r_in * S, C - r_in * S, C + r_in * S, C + r_in * S], fill=(0, 0, 0, 0))

def subdial(img, d, cx, labels, ring_col=GREY):
    ox = (cx - 180) * S
    def P(r, deg):
        a = math.radians(deg); return C + ox + r * S * math.sin(a), C - r * S * math.cos(a)
    r = 36
    d.ellipse([C + ox - r * S, C - r * S, C + ox + r * S, C + r * S], fill=(12, 12, 14), outline=ring_col, width=int(1.2 * S))
    for i in range(60):                                   # fine track
        deg = i * 6; long_ = i % 5 == 0
        a0, a1 = (r - 7, r - 2) if long_ else (r - 4.5, r - 2)
        x0, y0 = P(a0, deg); x1, y1 = P(a1, deg)
        d.line([x0, y0, x1, y1], fill=WHITE if long_ else GREY, width=int((1.3 if long_ else 0.7) * S))
    f = font(AVENIR, 8.5, 1)
    for k, lab in enumerate(labels):
        x, y = P(r - 15, k * 90)
        text_center(d, (x, y), lab, f, WHITE)

def render(brand, script, line1, line2):
    img = Image.new("RGBA", (N, N), (0, 0, 0, 255))
    d = ImageDraw.Draw(img)

    # ---- bezel (r 180 -> 150): ceramic black with an orange 0-15 minute section ----
    ring(d, 180, 0, CERAMIC)
    d.pieslice([C - 176 * S, C - 176 * S, C + 176 * S, C + 176 * S], -90, 0, fill=ORANGE)
    ring(d, 151, 0, (0, 0, 0))
    for m in range(60):                                    # bezel minute scale
        deg = m * 6
        if m == 0: continue
        if m <= 15:                                        # dark marks cut into the orange section
            if m % 5 == 0: quad(d, 152, 176, deg, 3.0, CERAMIC)
            else: quad(d, 160, 176, deg, 2.2, CERAMIC)
        elif m % 10 == 0:
            rotated_text(img, str(m), font(DIN, 24), (214, 216, 218), 165, deg)
        elif m % 5 == 0:
            quad(d, 157, 173, deg, 3.0, (205, 207, 210))
        else:
            quad(d, 166, 173, deg, 1.6, (150, 152, 155))
    x, y = pol(170, 0)                                     # 12 o'clock triangle + lume pip
    tri = [pol(156, 0), pol(176, -4.6), pol(176, 4.6)]
    d.polygon(tri, fill=(230, 232, 234))
    d.polygon([pol(160, 0), pol(173, -2.9), pol(173, 2.9)], fill=CERAMIC)
    px_, py_ = pol(168.5, 0); rr = 2.6 * S
    d.ellipse([px_ - rr, py_ - rr, px_ + rr, py_ + rr], fill=LUME)
    # polished rim between bezel and dial
    d.ellipse([C - 151 * S, C - 151 * S, C + 151 * S, C + 151 * S], outline=(120, 122, 126), width=int(1.4 * S))

    # ---- dial (r < 149) ----
    dial = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    dd = ImageDraw.Draw(dial)
    dd.ellipse([C - 149 * S, C - 149 * S, C + 149 * S, C + 149 * S], fill=DARK)
    img.alpha_composite(dial)
    # minute track (r 146 -> 139)
    for m in range(60):
        deg = m * 6
        if m % 5 == 0: continue
        quad(d, 140, 146, deg, 1.4, (205, 205, 205))
    # applied hour indices (white lume with a grey frame); 12 = orange numerals, 6 = date window + short index
    for h in range(12):
        deg = h * 30
        if h == 0:
            text_center(d, pol(120, 0), "12", font(DIN, 30), ORANGE)
            continue
        if h == 6:
            quad(d, 128, 138, deg, 7, LUME, outline=(110, 112, 116))
            continue
        if h in (3, 9):
            quad(d, 122, 138, deg, 7.5, LUME, outline=(110, 112, 116)); continue
        quad(d, 112, 138, deg, 8, LUME, outline=(110, 112, 116))

    # subdials at 9 (small seconds) and 3 (24-hour)
    subdial(img, d, 180 - 62, ["60", "15", "30", "45"])
    subdial(img, d, 180 + 62, ["24", "6", "12", "18"])

    # dial text
    text_center(d, (C, C - 70 * S), brand, font(AVENIR, 14, 2), WHITE)
    text_center(d, (C, C - 53 * S), script, font(SCRIPT, 16, 1), ORANGE)
    text_center(d, (C, C + 50 * S), line1, font(AVENIR, 7.5, 2), WHITE)
    text_center(d, (C, C + 61 * S), line2, font(AVENIR, 7.5, 2), ORANGE)

    # date window frame at 6 (the digits are drawn live by the firmware)
    wx0, wy0, wx1, wy1 = C - 11 * S, C + 79 * S, C + 11 * S, C + 98 * S
    d.rectangle([wx0 - 1.5 * S, wy0 - 1.5 * S, wx1 + 1.5 * S, wy1 + 1.5 * S], fill=(120, 122, 126))
    d.rectangle([wx0, wy0, wx1, wy1], fill=(245, 245, 243))

    # round mask (the panel is round) + downsample
    mask = Image.new("L", (N, N), 0); ImageDraw.Draw(mask).ellipse([0, 0, N - 1, N - 1], fill=255)
    out = Image.new("RGBA", (N, N), (0, 0, 0, 255)); out.paste(img, (0, 0), mask)
    return out.resize((360, 360), Image.LANCZOS).convert("RGB")

def hands_preview(dial, hh=10, mm=9, ss=37, day=26):
    """Approximate the firmware's live hands for a preview image."""
    im = dial.convert("RGBA").resize((N, N), Image.BICUBIC)
    d = ImageDraw.Draw(im)
    def sword(deg, length, width, tail, col, lume=True):
        a = math.radians(deg); ux, uy = math.sin(a), -math.cos(a); px, py = -uy, ux
        L, W, T = length * S, width * S / 2, tail * S
        tip = (C + ux * L, C + uy * L)
        sh = (C + ux * L * 0.80, C + uy * L * 0.80)
        pts = [(C - ux * T + px * W * 0.5, C - uy * T + py * W * 0.5), (sh[0] + px * W, sh[1] + py * W), tip,
               (sh[0] - px * W, sh[1] - py * W), (C - ux * T - px * W * 0.5, C - uy * T - py * W * 0.5)]
        d.polygon(pts, fill=(200, 202, 206))
        if lume:
            ins = [(C + ux * L * 0.22 + px * W * 0.45, C + uy * L * 0.22 + py * W * 0.45),
                   (sh[0] + px * W * 0.62, sh[1] + py * W * 0.62), (C + ux * L * 0.93, C + uy * L * 0.93),
                   (sh[0] - px * W * 0.62, sh[1] - py * W * 0.62),
                   (C + ux * L * 0.22 - px * W * 0.45, C + uy * L * 0.22 - py * W * 0.45)]
            d.polygon(ins, fill=LUME)
    # date
    text_center(d, (C, C + 88.5 * S), str(day), font(DIN, 15), (10, 10, 10))
    # subdial hands
    for cx, deg, col in ((180 - 62, ss * 6, WHITE), (180 + 62, (hh % 24) * 15 + mm * 0.25, ORANGE)):
        ox = (cx - 180) * S; a = math.radians(deg)
        d.line([C + ox, C, C + ox + 28 * S * math.sin(a), C - 28 * S * math.cos(a)], fill=col, width=int(2.2 * S))
        rr = 3 * S; d.ellipse([C + ox - rr, C - rr, C + ox + rr, C + rr], fill=col)
    sword((hh % 12) * 30 + mm * 0.5, 82, 15, 14, WHITE)
    sword(mm * 6 + ss * 0.1, 124, 12, 16, WHITE)
    a = math.radians(ss * 6); ux, uy = math.sin(a), -math.cos(a)
    d.line([C - ux * 30 * S, C - uy * 30 * S, C + ux * 140 * S, C + uy * 140 * S], fill=WHITE, width=int(2 * S))
    d.line([C + ux * 118 * S, C + uy * 118 * S, C + ux * 140 * S, C + uy * 140 * S], fill=ORANGE, width=int(2.4 * S))
    rr = 6 * S; d.ellipse([C - rr, C - rr, C + rr, C + rr], fill=(200, 202, 206))
    rr = 2.5 * S; d.ellipse([C - rr, C - rr, C + rr, C + rr], fill=(40, 40, 42))
    return im.resize((360, 360), Image.LANCZOS).convert("RGB")

def write_header(img, path):
    px = img.load(); data = bytearray()
    for y in range(360):
        for x in range(360):
            r, g, b = px[x, y]; v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            data += bytes((v & 0xFF, v >> 8))
    with open(path, "w") as f:
        f.write("#pragma once\n// GENERATED by tools/render_watch_dial.py: 360x360 RGB565 watch dial for LVGL 9.\n#include <lvgl.h>\n")
        f.write("static const uint8_t watch_dial_map[] = {\n")
        for i in range(0, len(data), 24): f.write("  " + ",".join(f"0x{b:02x}" for b in data[i:i + 24]) + ",\n")
        f.write("};\n")
        f.write("static const lv_image_dsc_t watch_dial = {\n  .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565, .flags = 0, .w = 360, .h = 360, .stride = 720, .reserved_2 = 0 },\n")
        f.write("  .data_size = sizeof(watch_dial_map), .data = watch_dial_map, .reserved = NULL\n};\n")

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--brand", default="CHARLIE"); ap.add_argument("--script", default="Chief of Staff")
    ap.add_argument("--line1", default="DESK CHRONOMETER"); ap.add_argument("--line2", default="EST. 2026")
    ap.add_argument("--out", default="."); a = ap.parse_args()
    dial = render(a.brand, a.script, a.line1, a.line2)
    os.makedirs(a.out, exist_ok=True)
    dial.save(os.path.join(a.out, "watch_dial.png"))
    hands_preview(dial).save(os.path.join(a.out, "watch_preview.png"))
    write_header(dial, os.path.join(a.out, "watch_dial.h"))
    print("wrote", a.out)
