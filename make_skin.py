#!/usr/bin/env python3
"""Build FET Compressor's hand-drawn front-panel skin (skins/): faceplate, knobs, buttons, VU meter.

    python3 make_skin.py
    python3 "$MPC_SDK/tools/build.py" .      (copies skins/ verbatim)

Everything is rendered here with Pillow (pip install pillow); no third-party artwork. The layout
follows the classic rack FET limiter: big Input and Output knobs, small Attack/Release knobs,
a vertical ratio-button column, a backlit VU meter and meter buttons. Our extras (Color, Mix,
SC HPF) sit on a lower strip. Branding is Segeda's own. The engraved lettering uses Michroma
(dev/fonts, SIL Open Font License 1.1); everything else uses Pillow's built-in font.

Parameter indices must match FETComp.cpp. SPDX-License-Identifier: MIT (Segeda)
"""

import json
import math
import os
import random

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
SKINS = os.path.join(HERE, "skins")
SS = 2                                     # supersampling for anti-aliasing

# Parameter indices (FETComp.cpp ParamIndex)
P_OUTPUT, P_INPUT, P_ATTACK, P_RELEASE, P_RATIO, P_ALL, P_MIX, P_METER, P_HPF, P_COLOR, P_NEEDLE = range(11)

TAB_W, TAB_H = 1280, 628
EAR = 28                                   # rack-ear width each side
KNOB_FRAMES = 64
SWEEP = 135.0                              # knob pointer travel either side of 12 o'clock

# Layout (tab coordinates)
BIG = {"Input": (150, 230), "Output": (395, 230)}            # knob centres
SMALL = {"Attack": (590, 168), "Release": (590, 352)}
MINI = {"Color": (205, 540), "Mix": (425, 540)}
RATIO_X, RATIO_Y = 741, 108          # centre x of the ratio buttons, top of the first
METER_BOX = (795, 96, 350, 183)      # VU meter image x, y, w, h
METER_BTN_X = 1201
HPF_X, HPF_Y = 508, 523              # first SC HPF button top-left

METER_FRAMES = 48                    # must equal kMeterFrames in FETComp.cpp
ZERO_VU = 10 ** (-3 / 20)

WHITE = (232, 230, 224)
GREY = (150, 148, 144)


def s(v):
    return int(round(v * SS))


def font(px):
    return ImageFont.load_default(size=max(6, s(px)))


BRAND_FONT = os.path.join(HERE, "dev", "fonts", "Michroma-Regular.ttf")


def engraved_font(px, weight="Bold"):
    f = ImageFont.truetype(BRAND_FONT, s(px))
    try:
        f.set_variation_by_name(weight)
    except Exception:
        pass
    return f


def engrave(img, xy, t, px, spacing=0.0, weight="Bold", paint=(228, 224, 212)):
    """Paint-filled engraving on the faceplate: a cut shadow above-left, a highlight below-right."""
    f = engraved_font(px, weight)
    m = Image.new("L", img.size, 0)
    md = ImageDraw.Draw(m)
    widths = [md.textlength(ch, font=f) for ch in t]
    total = sum(widths) + s(spacing) * (len(t) - 1)
    x = s(xy[0]) - total / 2
    for ch, w in zip(t, widths):
        md.text((x, s(xy[1])), ch, font=f, fill=255, anchor="lm")
        x += w + s(spacing)
    off = max(1, s(px) // 28)
    shadow = ImageChops.offset(m, -off, -off).point(lambda v: v * 0.85)
    light = ImageChops.offset(m, off, off).point(lambda v: v * 0.35)
    img.paste((0, 0, 0), (0, 0), shadow)
    img.paste((150, 150, 150), (0, 0), light)
    w_, h_ = img.size
    grad = Image.linear_gradient("L").resize((w_, h_))
    fill = Image.merge("RGB", [grad.point(lambda v, c=c: int(c * (1.0 - 0.10 * v / 255))) for c in paint])
    img.paste(fill, (0, 0), m)


def text(d, xy, t, px, fill=WHITE, anchor="mm", spacing=0.0):
    if spacing <= 0:
        d.text((s(xy[0]), s(xy[1])), t, font=font(px), fill=fill, anchor=anchor)
        return
    f = font(px)
    widths = [d.textlength(ch, font=f) for ch in t]
    total = sum(widths) + s(spacing) * (len(t) - 1)
    x = s(xy[0]) - total / 2
    for ch, w in zip(t, widths):
        d.text((x, s(xy[1])), ch, font=f, fill=fill, anchor="lm")
        x += w + s(spacing)


def down(img, size):
    return img.resize(size, Image.LANCZOS)


# ============================================================================ faceplate
def brushed(size, base, strength):
    w, h = size
    noise = Image.effect_noise((max(1, w // 12), h), 64).resize((w, h), Image.BICUBIC)
    plate = Image.new("RGB", size, base)
    lift = Image.merge("RGB", (noise,) * 3)
    return ImageChops.add(plate, ImageChops.multiply(lift, Image.new("RGB", size, (strength,) * 3)), 1.0, -strength // 2)


def screw(d, cx, cy, r):
    d.ellipse([s(cx - r), s(cy - r), s(cx + r), s(cy + r)], fill=(70, 70, 72), outline=(20, 20, 20), width=s(1))
    d.ellipse([s(cx - r + 2), s(cy - r + 2), s(cx + r - 2), s(cy + r - 2)], fill=(120, 120, 124))
    a = math.radians(35)
    dx, dy = math.cos(a) * (r - 2), math.sin(a) * (r - 2)
    d.line([(s(cx - dx), s(cy - dy)), (s(cx + dx), s(cy + dy))], fill=(40, 40, 42), width=s(2))
    d.line([(s(cx + dy), s(cy - dx)), (s(cx - dy), s(cy + dx))], fill=(40, 40, 42), width=s(2))


def knob_scale(d, cx, cy, r_tick, r_num, labels, num_px, minor=True):
    n = len(labels)
    for i, lab in enumerate(labels):
        pos = i / (n - 1)
        a = math.radians(-SWEEP + 2 * SWEEP * pos)
        sx, sy = math.sin(a), -math.cos(a)
        d.line([(s(cx + sx * r_tick), s(cy + sy * r_tick)), (s(cx + sx * (r_tick + 7)), s(cy + sy * (r_tick + 7)))],
               fill=WHITE, width=s(2))
        if lab:
            text(d, (cx + sx * r_num, cy + sy * r_num), lab, num_px)
        if minor and i < n - 1:
            am = math.radians(-SWEEP + 2 * SWEEP * (pos + 0.5 / (n - 1)))
            mx, my = math.sin(am), -math.cos(am)
            d.ellipse([s(cx + mx * (r_tick + 3) - 1.5), s(cy + my * (r_tick + 3) - 1.5),
                       s(cx + mx * (r_tick + 3) + 1.5), s(cy + my * (r_tick + 3) + 1.5)], fill=GREY)


def render_faceplate(path):
    size = (s(TAB_W), s(TAB_H))
    img = brushed(size, (24, 24, 25), 26)
    d = ImageDraw.Draw(img)

    # Rack ears with mounting slots.
    for x0 in (0, TAB_W - EAR):
        d.rectangle([s(x0), 0, s(x0 + EAR) - 1, size[1]], fill=(30, 30, 31))
        edge = x0 + EAR if x0 == 0 else x0
        d.line([(s(edge), 0), (s(edge), size[1])], fill=(10, 10, 10), width=s(2))
        d.line([(s(edge + (1 if x0 == 0 else -1)), 0), (s(edge + (1 if x0 == 0 else -1)), size[1])], fill=(52, 52, 54), width=s(1))
        for y in (64, TAB_H - 64):
            d.rounded_rectangle([s(x0 + 7), s(y - 18), s(x0 + EAR - 7), s(y + 18)], radius=s(7), fill=(6, 6, 6))
    # Panel bevel.
    d.line([(s(EAR + 2), s(2)), (s(TAB_W - EAR - 2), s(2))], fill=(58, 58, 60), width=s(1))
    d.line([(s(EAR + 2), s(TAB_H - 3)), (s(TAB_W - EAR - 2), s(TAB_H - 3))], fill=(8, 8, 8), width=s(2))

    # Lower strip for the extras.
    d.rounded_rectangle([s(40), s(452), s(TAB_W - 40), s(614)], radius=s(6), fill=(17, 17, 18), outline=(44, 44, 46), width=s(1))
    d.line([(s(46), s(453)), (s(TAB_W - 46), s(453))], fill=(6, 6, 6), width=s(2))

    # Big knobs: INPUT / OUTPUT with dB scales.
    knob_scale(d, *BIG["Input"], 84, 102, ["-12", "-6", "0", "6", "12", "18", "24", "30", "36"], 15)
    knob_scale(d, *BIG["Output"], 84, 102, ["-24", "-18", "-12", "-6", "0", "6", "12", "18"], 15)
    for name, (cx, cy) in BIG.items():
        text(d, (cx, cy + 112), name.upper(), 17, spacing=3)

    # Small knobs: ATTACK / RELEASE.
    for name, (cx, cy) in SMALL.items():
        knob_scale(d, cx, cy, 52, 0, ["", "", "", "", "", "", ""], 11, minor=False)
        text(d, (cx, cy - 76), name.upper(), 15, spacing=2.5)
        for sign, lab in ((-1, "FAST"), (1, "SLOW")):
            a = math.radians(sign * SWEEP)
            text(d, (cx + math.sin(a) * 74, cy - math.cos(a) * 74 + 4), lab, 10, GREY)

    # Ratio column.
    text(d, (RATIO_X, RATIO_Y - 22), "RATIO", 15, spacing=2.5)
    for i, lab in enumerate(["20", "12", "8", "4"]):
        text(d, (RATIO_X - 48, RATIO_Y + 25 + i * 60), lab, 17)
    text(d, (RATIO_X - 48, 385), "ALL", 13, GREY)

    # Meter well and meter buttons.
    mx, my, mw, mh = METER_BOX
    d.rounded_rectangle([s(mx - 6), s(my - 6), s(mx + mw + 6), s(my + mh + 6)], radius=s(12), fill=(6, 6, 6))
    text(d, (METER_BTN_X, RATIO_Y - 22), "METER", 15, spacing=2.5)
    text(d, (mx + mw / 2, my + mh + 26), "PEAK  LIMITING  AMPLIFIER", 12, GREY, spacing=1.5)

    # Brand plate on the lower strip.
    engrave(img, (1010, 512), "SEGEDA", 44, spacing=9)
    engrave(img, (1010, 566), "FET COMPRESSOR", 17, spacing=4.5, weight="Regular", paint=(196, 192, 182))

    # Extras.
    for name, (cx, cy) in MINI.items():
        knob_scale(d, cx, cy, 44, 0, ["", "", "", "", "", ""], 10, minor=False)
        text(d, (cx - 92, cy - 4), name.upper(), 15, spacing=2.5, anchor="mm")
    text(d, (HPF_X + 115, 476), "SC HPF", 15, spacing=2.5)
    for i, lab in enumerate(["OFF", "60", "120", "200"]):
        text(d, (HPF_X + 25 + i * 60, 506), lab, 12, GREY)

    for cx, cy in ((EAR + 14, 14), (TAB_W - EAR - 14, 14), (EAR + 14, TAB_H - 14), (TAB_W - EAR - 14, TAB_H - 14)):
        screw(d, cx, cy, 7)

    down(img, (TAB_W, TAB_H)).save(path, optimize=True)


# ============================================================================ knobs
def radial_disc(size, r, c_hi, c_lo, hi_offset=(-0.35, -0.45)):
    """A shaded disc (light from top-left) as RGBA, diameter 2r, centred in `size`."""
    w, h = size
    cx, cy = w / 2, h / 2
    small = 64
    g = Image.new("RGB", (small, small))
    px = g.load()
    for y in range(small):
        for x in range(small):
            u, v = (x + 0.5) / small * 2 - 1, (y + 0.5) / small * 2 - 1
            t = min(1.0, math.hypot(u - hi_offset[0], v - hi_offset[1]) / 1.6)
            px[x, y] = tuple(int(c_hi[i] + (c_lo[i] - c_hi[i]) * t) for i in range(3))
    g = g.resize((int(2 * r), int(2 * r)), Image.BICUBIC)
    out = Image.new("RGBA", size, (0, 0, 0, 0))
    m = Image.new("L", g.size, 0)
    ImageDraw.Draw(m).ellipse([0, 0, g.size[0] - 1, g.size[1] - 1], fill=255)
    out.paste(g, (int(cx - r), int(cy - r)), m)
    return out


def render_knob(path, dia, cap_ratio, skirt=True):
    """Black knob (knurled skirt) with a turned-metal cap and a white pointer; KNOB_FRAMES frames."""
    pad = 6
    size_px = dia + 2 * pad
    size = (s(size_px), s(size_px))
    c = size[0] / 2
    R = s(dia / 2)

    static = Image.new("RGBA", size, (0, 0, 0, 0))
    shadow = Image.new("L", size, 0)
    ImageDraw.Draw(shadow).ellipse([c - R + s(1), c - R + s(4), c + R + s(1), c + R + s(4)], fill=170)
    shadow = shadow.filter(ImageFilter.GaussianBlur(s(4)))
    static = Image.alpha_composite(static, Image.merge("RGBA", (Image.new("L", size, 0),) * 3 + (shadow,)))
    static = Image.alpha_composite(static, radial_disc(size, R, (70, 70, 72), (8, 8, 9)))
    inner = R * (0.86 if skirt else 0.92)
    static = Image.alpha_composite(static, radial_disc(size, inner, (44, 44, 46), (12, 12, 13), (-0.2, -0.3)))
    cap = R * cap_ratio
    capimg = radial_disc(size, cap, (236, 236, 238), (110, 110, 116), (-0.45, -0.55))
    cd = ImageDraw.Draw(capimg)
    for k in range(3, int(cap), s(3)):                 # turned-metal rings
        shade = 20 if (k // s(3)) % 2 else -14
        col = (128 + shade, 128 + shade, 132 + shade, 38)
        cd.ellipse([c - k, c - k, c + k, c + k], outline=col, width=1)
    cd.ellipse([c - cap, c - cap, c + cap, c + cap], outline=(30, 30, 32, 255), width=s(1.5))
    static = Image.alpha_composite(static, capimg)

    strip = Image.new("RGBA", (size_px, size_px * KNOB_FRAMES), (0, 0, 0, 0))
    for k in range(KNOB_FRAMES):
        fr = static.copy()
        d = ImageDraw.Draw(fr)
        a0 = math.radians(-SWEEP + 2 * SWEEP * k / (KNOB_FRAMES - 1))
        if skirt:                                      # knurling turns with the knob
            for j in range(48):
                a = a0 + j * 2 * math.pi / 48
                x0, y0 = c + math.sin(a) * (R - s(1)), c - math.cos(a) * (R - s(1))
                x1, y1 = c + math.sin(a) * (inner + s(1)), c - math.cos(a) * (inner + s(1))
                d.line([(x0, y0), (x1, y1)], fill=(0, 0, 0, 150), width=s(1))
        sx, sy = math.sin(a0), -math.cos(a0)
        r0, r1 = cap + s(3), R - s(3)
        d.line([(c + sx * r0, c + sy * r0), (c + sx * r1, c + sy * r1)], fill=(245, 245, 240, 255), width=s(3 if dia > 120 else 2.4))
        strip.paste(down(fr, (size_px, size_px)), (0, k * size_px))
    strip.save(path, optimize=True)
    return size_px


# ============================================================================ buttons
def render_button(path, w, h, on, caption=None):
    size = (s(w), s(h))
    img = Image.new("RGBA", size, (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([0, 0, size[0] - 1, size[1] - 1], radius=s(5), fill=(4, 4, 4, 255))
    inset = 2 if on else 0
    body = [s(2 + inset), s(2 + inset), size[0] - s(3 + inset), size[1] - s(3 + inset)]
    if on:
        glow = radial_disc(size, max(size) * 0.62, (255, 196, 96), (188, 92, 18), (0.0, -0.2))
        m = Image.new("L", size, 0)
        ImageDraw.Draw(m).rounded_rectangle(body, radius=s(4), fill=255)
        img.paste(glow, (0, 0), m)
        d.line([(body[0] + s(3), body[1] + s(1)), (body[2] - s(3), body[1] + s(1))], fill=(255, 236, 190, 200), width=s(1))
        cap_col = (40, 22, 6, 255)
    else:
        d.rounded_rectangle(body, radius=s(4), fill=(40, 40, 42, 255))
        d.rounded_rectangle([body[0] + s(2), body[1] + s(2), body[2] - s(2), body[3] - s(3)], radius=s(3), fill=(26, 26, 27, 255))
        d.line([(body[0] + s(3), body[1] + s(1)), (body[2] - s(3), body[1] + s(1))], fill=(84, 84, 88, 255), width=s(1))
        cap_col = (210, 208, 200, 255)
    if caption:
        d.text((size[0] / 2, size[1] / 2 + s(inset / 2)), caption, font=font(15), fill=cap_col, anchor="mm")
    down(img, (w, h)).save(path, optimize=True)


# ============================================================================ VU meter
METER_DESIGN = (500, 262)            # drawing coordinates; output is METER_BOX size
PIVOT = (250.0, 300.0)
M_SWEEP = 46.0


def m_polar(r, pos):
    a = math.radians(-M_SWEEP + 2 * M_SWEEP * pos)
    return PIVOT[0] + r * math.sin(a), PIVOT[1] - r * math.cos(a)


def vu_pos(db):
    return 10 ** (db / 20) * ZERO_VU


def m_arc(d, r, p0, p1, width, fill, steps=120):
    pts = [m_polar(r, p0 + (p1 - p0) * i / steps) for i in range(steps + 1)]
    d.line([(s(x), s(y)) for x, y in pts], fill=fill, width=s(width), joint="curve")


def meter_layers():
    W, H = METER_DESIGN
    size = (s(W), s(H))
    win = (12, 12, W - 12, H - 12)
    base = Image.new("RGB", size, (8, 8, 8))
    d = ImageDraw.Draw(base)
    d.rounded_rectangle([0, 0, size[0] - 1, size[1] - 1], radius=s(16), fill=(34, 34, 36))
    d.rounded_rectangle([s(4), s(4), size[0] - s(5), size[1] - s(5)], radius=s(13), fill=(14, 14, 15))
    face = radial_disc(size, s(330), (255, 222, 150), (206, 124, 40), (0.0, 0.35)).convert("RGB")
    mask = Image.new("L", size, 0)
    ImageDraw.Draw(mask).rounded_rectangle([s(v) for v in win], radius=s(9), fill=255)
    base.paste(face, (0, 0), mask)
    d = ImageDraw.Draw(base)
    ink, red = (30, 20, 10), (200, 34, 22)

    r_sc = 205.0
    m_arc(d, r_sc, vu_pos(-20), vu_pos(0), 2.4, ink)
    m_arc(d, r_sc + 4, vu_pos(0), 1.0, 9.0, red)
    marks = [(-20, True), (-10, True), (-7, True), (-5, True), (-3, True), (-2, True), (-1, True),
             (0, True), (1, True), (2, True), (3, True), (-15, False), (-6, False), (-4, False), (-0.5, False),
             (0.5, False), (1.5, False), (2.5, False)]
    for db, major in marks:
        p = vu_pos(db)
        col = red if db > 0 else ink
        (x0, y0), (x1, y1) = m_polar(r_sc, p), m_polar(r_sc + (17 if major else 9), p)
        d.line([(s(x0), s(y0)), (s(x1), s(y1))], fill=col, width=s(2.6 if major else 1.6))
        if major:
            lx, ly = m_polar(r_sc + 34, p)
            lab = ("+%g" % db) if db > 0 else ("%g" % abs(db))
            d.text((s(lx), s(ly)), lab, font=font(20), fill=col, anchor="mm")
    r_pct = 156.0
    m_arc(d, r_pct, 0.0, ZERO_VU, 1.6, ink)
    for pct in range(0, 101, 20):
        p = pct / 100 * ZERO_VU
        (x0, y0), (x1, y1) = m_polar(r_pct, p), m_polar(r_pct + 9, p)
        d.line([(s(x0), s(y0)), (s(x1), s(y1))], fill=ink, width=s(1.6))
        lx, ly = m_polar(r_pct + 22, p)
        d.text((s(lx), s(ly)), str(pct), font=font(13), fill=ink, anchor="mm")
    d.text((s(44), s(40)), "VU", font=font(24), fill=ink, anchor="mm")
    d.text((s(W - 44), s(40)), "VU", font=font(24), fill=red, anchor="mm")

    over = Image.new("RGBA", size, (0, 0, 0, 0))
    od = ImageDraw.Draw(over)
    hub = 92
    od.ellipse([s(PIVOT[0] - hub), s(PIVOT[1] - hub), s(PIVOT[0] + hub), s(PIVOT[1] + hub)], fill=(26, 20, 14, 255))
    od.arc([s(PIVOT[0] - hub + 2), s(PIVOT[1] - hub + 2), s(PIVOT[0] + hub - 2), s(PIVOT[1] + hub - 2)],
           start=215, end=325, fill=(110, 90, 60, 255), width=s(2))
    od.text((s(PIVOT[0]), s(PIVOT[1] - hub + 36)), "SEGEDA", font=engraved_font(12), fill=(150, 120, 70, 255), anchor="mm")
    glass = Image.new("L", size, 0)
    ImageDraw.Draw(glass).polygon([(s(12), s(12)), (s(W * 0.6), s(12)), (s(W * 0.28), s(H * 0.55)), (s(12), s(H * 0.55))], fill=40)
    glass = ImageChops.multiply(glass.filter(ImageFilter.GaussianBlur(s(18))), mask)
    over = Image.alpha_composite(over, Image.merge("RGBA", (Image.new("L", size, 255),) * 3 + (glass,)))
    edge = Image.new("L", size, 0)
    ImageDraw.Draw(edge).rounded_rectangle([s(v) for v in win], radius=s(9), outline=200, width=s(12))
    edge = ImageChops.multiply(edge.filter(ImageFilter.GaussianBlur(s(8))), mask).point(lambda v: int(v * 0.6))
    over = Image.alpha_composite(over, Image.merge("RGBA", (Image.new("L", size, 30),) * 3 + (edge,)))
    frame_mask = ImageChops.invert(mask)
    bez = base.convert("RGBA")
    bez.putalpha(frame_mask)
    over = Image.alpha_composite(over, bez)
    return base, over, mask


def render_meter(path):
    base, over, mask = meter_layers()
    size = base.size
    ow, oh = METER_BOX[2], METER_BOX[3]
    strip = Image.new("RGB", (ow, oh * METER_FRAMES))
    for k in range(METER_FRAMES):
        pos = k / (METER_FRAMES - 1)
        fr = base.convert("RGBA")
        (bx, by), (tx, ty) = m_polar(60, pos), m_polar(226, pos)
        sh = Image.new("L", size, 0)
        ImageDraw.Draw(sh).line([(s(bx + 4), s(by + 5)), (s(tx + 4), s(ty + 5))], fill=120, width=s(4))
        sh = ImageChops.multiply(sh.filter(ImageFilter.GaussianBlur(s(3))), mask)
        fr = Image.alpha_composite(fr, Image.merge("RGBA", (Image.new("L", size, 40),) * 3 + (sh,)))
        ImageDraw.Draw(fr).line([(s(bx), s(by)), (s(tx), s(ty))], fill=(16, 10, 6, 255), width=s(3.4))
        fr = Image.alpha_composite(fr, over)
        strip.paste(down(fr.convert("RGB"), (ow, oh)), (0, k * oh))
    strip.save(path, optimize=True)


# ============================================================================ TUI.json
def bounds(x, y, w, h, focus="No", vis="Always", invalid="Show"):
    return {"version": 1, "acceptsHWFocus": focus, "showWhenDataModelInvalid": invalid, "whenVisible": vis,
            "boundsType": "Absolute", "bounds": "%d %d %d %d" % (x, y, w, h)}


def entry(name, ctype, data, rect, remap=None, **kw):
    return {"version": 2, "componentData": {"version": 1, "name": name, "type": ctype, "data": data},
            "handle remapping": {"version": 1, "map": remap or []}, "bounds": bounds(*rect, **kw)}


def act(on, handler, extra=""):
    return {"version": 1, "onAction": on, "handler": handler, "handleName": "Data", "additionalData": extra}


TRANSPARENT = {"version": 1, "focussed": {"version": 1, "colour": "00000000", "image": ""},
               "unfocussed": {"version": 1, "colour": "00000000", "image": ""}}


def focus_entry(w, h):
    return entry("focus", "Focus", {"version": 1, "backgroundColour": "14ffffff", "outlineColour": "ffffb04a",
                                    "backgroundInset": 4.0, "outlineThickness": 1.5}, (0, 0, w, h), vis="WhenFocussed")


def value_label(rect, px=22):
    return entry("value", "Label", {"version": 1, "textStyle": {"version": 1, "font": {"version": 1, "name": "Titillium Web", "style": "Regular", "height": float(px)},
                 "colour": "ffd8d4cc", "justification": "horizontallyCentred verticallyCentred", "case": "Original"},
                 "type": "Value", "handleName": "Data"}, rect)


def custom(components, knob=True):
    actions = [act("Mouse Down", "Q-Link")]
    if knob:
        actions.append(act("Double Click", "Show Overlay", "knob overlay"))
    actions.append(act("Enter Pressed", "Show Overlay", "knob overlay"))
    return {"version": 2, "actions": actions, "backgroundData": TRANSPARENT, "ignoreMousePresses": False,
            "disableCoarseDataWheel": False, "componentsData": components}


def knob_def(w, h, strip, strip_px, knob_xy, value_rect, value_px):
    k = entry("knob", "Knob", {"version": 2, "knobType": "FilmStrip", "filmStrip": strip, "numFrames": KNOB_FRAMES,
                               "invert": False, "handleName": "Data"}, (knob_xy[0], knob_xy[1], strip_px, strip_px))
    return custom([focus_entry(w, h), k, value_label(value_rect, value_px)])


def button(name, on, off, bid, n, rect):
    return entry(name, "Button", {"version": 2, "onImage": on, "offImage": off, "buttonId": bid, "numButtonsInGroup": n,
                                  "handleName": "Data", "gestureBehaviour": "Instant"}, rect)


def overlay_def(kind):
    data = ({"version": 1, "actions": [{"version": 1, "onAction": "Focus Lost", "handler": "Focus Lost", "handleName": "Focus Lost", "additionalData": ""}],
             "overlayType": "Knob With Header", "scaleType": "Linear", "polarityType": "Unipolar", "showMinusSign": True, "handleName": "Data"})
    return {"version": 2, "actions": [], "backgroundData": {"version": 1, "focussed": {"version": 1, "colour": "ff1e1d1e", "image": ""},
            "unfocussed": {"version": 1, "colour": "ff1e1d1e", "image": ""}}, "ignoreMousePresses": True, "disableCoarseDataWheel": False,
            "componentsData": [entry("overlay", kind, data, (0, 0, 1280, 800))]}


def build_tui(big_px, small_px, mini_px):
    BW, BH = 220, 290                     # big knob component
    SW, SH = 170, 165
    MW, MH = 150, 120
    defs = {
        "bigKnob": knob_def(BW, BH, "knob_big.png", big_px, ((BW - big_px) // 2, 40), (0, 255, BW, 30), 22),
        "smallKnob": knob_def(SW, SH, "knob_small.png", small_px, ((SW - small_px) // 2, 40), (0, 126, SW, 28), 19),
        "miniKnob": knob_def(MW, MH, "knob_mini.png", mini_px, ((MW - mini_px) // 2, 8), (0, 90, MW, 26), 18),
        "ratioStack": custom([focus_entry(90, 252)] + [button("r%d" % i, "btn_on.png", "btn_off.png", 3 - i, 4, (16, 8 + i * 60, 58, 50)) for i in range(4)], knob=False),
        "meterStack": custom([focus_entry(90, 132), button("gr", "btn_gr_on.png", "btn_gr_off.png", 0, 2, (16, 8, 58, 50)),
                              button("out", "btn_out_on.png", "btn_out_off.png", 1, 2, (16, 68, 58, 50))], knob=False),
        "toggleBtn": custom([focus_entry(90, 66), button("t", "btn_on.png", "btn_off.png", 1, 1, (16, 8, 58, 50))], knob=False),
        "hpfRow": custom([focus_entry(252, 52)] + [button("h%d" % i, "btn_s_on.png", "btn_s_off.png", i, 4, (8 + i * 60, 8, 50, 36)) for i in range(4)], knob=False),
        "knob overlay": overlay_def("KnobOverlay"),
    }

    def place(name, ctype, rect, param):
        return entry(name, ctype, {"version": 1, "handleName": "Data"}, rect,
                     [{"key": "Data", "value": "Parameter %d" % param}], focus="Yes", invalid="Hide")

    comps = [entry("faceplate", "Image", {"version": 2, "imageType": "Regular", "colour": "0", "image": "faceplate.png"}, (0, 0, TAB_W, TAB_H))]
    mx, my, mw, mh = METER_BOX
    comps.append(entry("GR Meter", "Image", {"version": 2, "imageType": "FilmStrip", "colour": "0", "image": "vu_meter.png",
                                             "numFrames": METER_FRAMES, "handleName": "Data"}, (mx, my, mw, mh),
                       [{"key": "Data", "value": "Parameter %d" % P_NEEDLE}]))
    for name, p in (("Input", P_INPUT), ("Output", P_OUTPUT)):
        cx, cy = BIG[name]
        comps.append(place(name, "bigKnob", (cx - BW // 2, cy - 40 - big_px // 2, BW, BH), p))
    for name, p in (("Attack", P_ATTACK), ("Release", P_RELEASE)):
        cx, cy = SMALL[name]
        comps.append(place(name, "smallKnob", (cx - SW // 2, cy - 40 - small_px // 2, SW, SH), p))
    for name, p in (("Color", P_COLOR), ("Mix", P_MIX)):
        cx, cy = MINI[name]
        comps.append(place(name, "miniKnob", (cx - MW // 2, cy - 8 - mini_px // 2, MW, MH), p))
    comps.append(place("Ratio", "ratioStack", (RATIO_X - 45, RATIO_Y - 8, 90, 252), P_RATIO))
    comps.append(place("All Buttons", "toggleBtn", (RATIO_X - 45, 352, 90, 66), P_ALL))
    comps.append(place("Meter", "meterStack", (METER_BTN_X - 45, RATIO_Y - 8, 90, 132), P_METER))
    comps.append(place("SC HPF", "hpfRow", (HPF_X - 8, HPF_Y - 8, 252, 52), P_HPF))

    tui = {"pageData": {
        "version": 1,
        "componentDefinitions": {"version": 1, "importFiles": [],
                                 "localComponentDefinitions": [{"key": k, "value": v} for k, v in defs.items()]},
        "replacementStrings": [
            {"key": "Parameter %d" % P_RATIO, "value": ["4:1", "8:1", "12:1", "20:1"]},
            {"key": "Parameter %d" % P_ALL, "value": ["Off", "On"]},
            {"key": "Parameter %d" % P_METER, "value": ["GR", "Out"]},
            {"key": "Parameter %d" % P_HPF, "value": ["Off", "60 Hz", "120 Hz", "200 Hz"]},
        ],
        "info": {"version": 1, "type": "CompleteDescription"},
        "tabs": [{
            "version": 1, "tabName": "FET", "fnKeyIndex": 0, "fnKeySubIndex": 0,
            "componentDefinition": {"version": 2, "actions": [], "backgroundData": {"version": 1,
                                    "focussed": {"version": 1, "colour": "ff141415", "image": ""},
                                    "unfocussed": {"version": 1, "colour": "ff141415", "image": ""}},
                                    "ignoreMousePresses": False, "disableCoarseDataWheel": False, "componentsData": comps},
            # Q-Link groups (RowTopDown): main knobs, ratio + meter, extras, (unused)
            "qlinkBoundsData": ["36 66 650 372", "690 66 556 372", "40 452 720 162", "40 452 720 162"],
        }],
    }}
    return tui


def build_qlinks():
    m = {"Q-Link %d" % i: -1 for i in range(1, 17)}
    m.update({"Q-Link 13": P_INPUT, "Q-Link 14": P_OUTPUT, "Q-Link 15": P_ATTACK, "Q-Link 16": P_RELEASE,
              "Q-Link 9": P_RATIO, "Q-Link 10": P_ALL, "Q-Link 11": P_METER,
              "Q-Link 5": P_COLOR, "Q-Link 6": P_MIX, "Q-Link 7": P_HPF})
    prog = {"Q-Link %d" % i: (i - 1 if i - 1 < P_NEEDLE else -1) for i in range(1, 17)}
    return {"version": 4, "info": {"version": 1, "type": "CompleteDescription"},
            "Screen Mode Q-Links": {"version": 4, "map": [{"Tab": 1, "SubTab": 1, "Bank Direction": "RowTopDown", "Q-Links": m}]},
            "Program Mode Q-Links": prog}


def main():
    random.seed(7)
    if os.path.isdir(SKINS):
        for f in os.listdir(SKINS):
            os.remove(os.path.join(SKINS, f))
    os.makedirs(SKINS, exist_ok=True)
    render_faceplate(os.path.join(SKINS, "faceplate.png"))
    big = render_knob(os.path.join(SKINS, "knob_big.png"), 150, 0.60)
    small = render_knob(os.path.join(SKINS, "knob_small.png"), 84, 0.42)
    mini = render_knob(os.path.join(SKINS, "knob_mini.png"), 70, 0.42)
    for on in (True, False):
        sfx = "on" if on else "off"
        render_button(os.path.join(SKINS, "btn_%s.png" % sfx), 58, 50, on)
        render_button(os.path.join(SKINS, "btn_gr_%s.png" % sfx), 58, 50, on, "GR")
        render_button(os.path.join(SKINS, "btn_out_%s.png" % sfx), 58, 50, on, "OUT")
        render_button(os.path.join(SKINS, "btn_s_%s.png" % sfx), 50, 36, on)
    render_meter(os.path.join(SKINS, "vu_meter.png"))
    with open(os.path.join(SKINS, "TUI.json"), "w") as f:
        json.dump(build_tui(big, small, mini), f, indent=4)
    with open(os.path.join(SKINS, "Q-Links.json"), "w") as f:
        json.dump(build_qlinks(), f, indent=4)
    print("skin written to", SKINS)


if __name__ == "__main__":
    main()
