#!/usr/bin/env python3
"""Renders the icon, splash screens and store art in branding/ from the app's
own fonts and colors, so they match the in-app logo and intro.

    pip install pillow && python3 tools/make-branding.py
"""
import math
import os

from PIL import Image, ImageChops, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONTS = os.path.join(ROOT, "content", "fonts")
OUT = os.path.join(ROOT, "branding")

BG_TOP, BG_BOTTOM = (0x12, 0x0E, 0x17), (0x07, 0x06, 0x0A)
ACCENT, ACCENT2 = (0xFF, 0xA2, 0x4C), (0xFF, 0x5F, 0x6D)
TEXT, TEXT2 = (0xF6, 0xF4, 0xF8), (0xA3, 0x9F, 0xAA)
CUP = (0x1A, 0x10, 0x16)
LOCAL_CAFE = ""
SS = 3  # supersampling factor


def font(name, size):
    return ImageFont.truetype(os.path.join(FONTS, name), int(size * SS))


def background(w, h):
    """Vertical gradient with two soft accent glows (like the app background)."""
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        t = y / max(1, h - 1)
        c = tuple(int(BG_TOP[i] + (BG_BOTTOM[i] - BG_TOP[i]) * t) for i in range(3))
        for x in range(w):
            px[x, y] = c
    return img.convert("RGBA")


def glow(img, cx, cy, rx, ry, color, alpha):
    """Radial glow fading to transparent at the ellipse edge."""
    w, h = img.size
    layer = Image.new("RGBA", (w, h), color + (0,))
    a = Image.new("L", (w, h), 0)
    ap = a.load()
    x0, x1 = max(0, int(cx - rx)), min(w, int(cx + rx) + 1)
    y0, y1 = max(0, int(cy - ry)), min(h, int(cy + ry) + 1)
    for y in range(y0, y1):
        dy = (y - cy) / ry
        for x in range(x0, x1):
            dx = (x - cx) / rx
            d = math.sqrt(dx * dx + dy * dy)
            if d < 1:
                ap[x, y] = int(255 * alpha * (1 - d) ** 2)
    layer.putalpha(a)
    img.alpha_composite(layer)


def hgrad(w, h, a, b):
    g = Image.new("RGBA", (w, h))
    px = g.load()
    for x in range(w):
        t = x / max(1, w - 1)
        c = tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3)) + (255,)
        for y in range(h):
            px[x, y] = c
    return g


def logo(img, cx, cy, r, shadow=True):
    """Accent gradient disc with the cup, drawn at SS resolution."""
    R = int(r * SS)
    size = 2 * R
    if shadow:
        sh = Image.new("RGBA", img.size, (0, 0, 0, 0))
        d = ImageDraw.Draw(sh)
        for i in range(16, 0, -1):
            rr = R + i * SS * r / 40
            d.ellipse([cx * SS - rr, cy * SS + R * 0.16 - rr, cx * SS + rr, cy * SS + R * 0.16 + rr],
                      fill=(0, 0, 0, int(14 * (1 - i / 17))))
        img.alpha_composite(sh)
    disc = hgrad(size, size, ACCENT, ACCENT2)
    mask = Image.new("L", (size, size), 0)
    ImageDraw.Draw(mask).ellipse([0, 0, size - 1, size - 1], fill=255)
    disc.putalpha(mask)
    img.alpha_composite(disc, (int(cx * SS - R), int(cy * SS - R)))

    f = ImageFont.truetype(os.path.join(FONTS, "MaterialIcons-Regular.ttf"), int(r * 1.18 * SS))
    d = ImageDraw.Draw(img)
    l, t, rr, b = d.textbbox((0, 0), LOCAL_CAFE, font=f)
    d.text((cx * SS - (l + rr) / 2, cy * SS - (t + b) / 2), LOCAL_CAFE, font=f, fill=CUP + (255,))


def wordmark(img, cx, top, size, center=True, x=None):
    """"Coffee" in white followed by "Flix" in the accent color."""
    f = font("Inter-Bold.otf", size)
    d = ImageDraw.Draw(img)
    w1 = d.textlength("Coffee", font=f)
    w2 = d.textlength("Flix", font=f)
    x0 = cx * SS - (w1 + w2) / 2 if center else x * SS
    d.text((x0, top * SS), "Coffee", font=f, fill=TEXT + (255,))
    d.text((x0 + w1, top * SS), "Flix", font=f, fill=ACCENT + (255,))
    return (w1 + w2) / SS


def text(img, x, y, s, size, color, weight="Inter-Medium.otf", anchor="la"):
    d = ImageDraw.Draw(img)
    d.text((x * SS, y * SS), s, font=font(weight, size), fill=color + (255,), anchor=anchor)


def canvas(w, h):
    return background(w * SS, h * SS)


def finish(img, w, h, name):
    out = img.resize((w, h), Image.LANCZOS)
    out.save(os.path.join(OUT, name), optimize=True)
    print("wrote", name, out.size)


TAGLINE = "Movies · Videos · Live · Radio · Podcasts"


def splash(w, h, name):
    # Same layout as the intro's resting frame at 1280x720, scaled.
    k = w / 1280
    img = canvas(w, h)
    glow(img, w / 2 * SS, 290 * k * SS, 420 * k * SS, 380 * k * SS, ACCENT, 0.30)
    glow(img, (w / 2 + 150 * k) * SS, (290 + 140) * k * SS, 350 * k * SS, 280 * k * SS, ACCENT2, 0.14)
    logo(img, w / 2, 290 * k, 64 * k)
    wordmark(img, w / 2, (290 + 64 + 34) * k, 60 * k)
    text(img, w / 2, (290 + 64 + 34 + 84) * k, TAGLINE, 21 * k, TEXT2, anchor="ma")
    finish(img, w, h, name)


def icon():
    n = 128
    img = canvas(n, n)
    glow(img, n / 2 * SS, n * 0.55 * SS, n * 0.62 * SS, n * 0.62 * SS, ACCENT, 0.35)
    logo(img, n / 2, n / 2 + 2, 46)
    finish(img, n, n, "icon.png")


def store_icon():
    w, h = 256, 150
    img = canvas(w, h)
    glow(img, w / 2 * SS, 58 * SS, 150 * SS, 120 * SS, ACCENT, 0.28)
    logo(img, w / 2, 56, 34)
    wordmark(img, w / 2, 97, 30)
    finish(img, w, h, "store_icon.png")


def store_screen():
    w, h = 848, 208
    img = canvas(w, h)
    glow(img, 150 * SS, 104 * SS, 320 * SS, 240 * SS, ACCENT, 0.30)
    glow(img, 700 * SS, 180 * SS, 360 * SS, 200 * SS, ACCENT2, 0.12)
    logo(img, 130, 104, 62)
    wordmark(img, 0, 50, 64, center=False, x=228)
    text(img, 230, 132, TAGLINE, 22, TEXT2)
    finish(img, w, h, "store_screen.png")


if __name__ == "__main__":
    icon()
    splash(1280, 720, "splash_tv.png")
    splash(854, 480, "splash_drc.png")
    store_icon()
    store_screen()
