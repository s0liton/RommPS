#!/usr/bin/env python3
"""RommPS's dashboard background, drawn from nothing but code.

    make-title-art.py OUT.png

A 3840x2160 picture for sce_sys/pic0.dds and pic1.dds (tools/title-art.sh
turns it into BC7): RommPS's Acrylic aurora, a shelf of abstract cover cards
easing into the distance, and the RommPS wordmark in Montserrat (OFL,
third_party/Montserrat-Medium.ttf). No game's art: every shape is drawn here.

SPDX-License-Identifier: GPL-3.0-or-later
"""
import math
import random
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

W, H = 3840, 2160
REPO = Path(__file__).resolve().parents[4]
FONT = REPO / "third_party/Montserrat-Medium.ttf"


def hex_rgb(value):
    return np.array([(value >> 16) & 255, (value >> 8) & 255, value & 255], dtype=np.float32)


def aurora():
    """The app's Acrylic backdrop: deep navy, with blue and violet light."""
    y, x = np.mgrid[0:H, 0:W].astype(np.float32)
    u, v = x / W, y / H
    base = hex_rgb(0x070a1c) * (1 - v[..., None]) + hex_rgb(0x0b1026) * v[..., None]
    img = base.copy()
    # Soft lights: centre, radius, colour, strength.
    for cx, cy, r, colour, k in [
        (0.78, 0.18, 0.55, 0x3a5bd9, 0.85),
        (0.95, 0.70, 0.45, 0xc04bd6, 0.55),
        (0.30, 0.95, 0.60, 0x1b1f4a, 0.90),
        (0.55, 0.45, 0.35, 0x76d6ff, 0.18),
    ]:
        d = np.sqrt(((u - cx) * (W / H)) ** 2 + (v - cy) ** 2) / r
        fall = np.exp(-d * d * 2.2)[..., None] * k
        img = img * (1 - fall) + hex_rgb(colour) * fall
    # Aurora ribbons: sine bands across the upper half.
    for phase, colour, k in [(0.0, 0x76d6ff, 0.10), (1.7, 0xc04bd6, 0.08)]:
        band = 0.30 + 0.06 * np.sin(u * 5.0 + phase) + 0.03 * np.sin(u * 13.0 + phase * 2)
        d = (v - band) / 0.05
        fall = (np.exp(-d * d) * (0.4 + 0.6 * u))[..., None] * k
        img = img + hex_rgb(colour) * fall
    # A whisper of grain, so the gradients never band.
    rng = np.random.default_rng(7)
    img += rng.normal(0, 1.2, img.shape).astype(np.float32)
    return Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), "RGB").convert("RGBA")


def cards(canvas):
    """A shelf of cover-shaped cards on the right, receding and fading."""
    rnd = random.Random(42)
    hues = [0x3a5bd9, 0xc04bd6, 0x76d6ff, 0x4ade80, 0xfbbf24, 0xf87171, 0x8b5cf6, 0x22d3ee]
    glow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    gd, ld = ImageDraw.Draw(glow), ImageDraw.Draw(layer)
    base_y = 1380
    for i in range(9):
        t = i / 8
        scale = 1.0 - 0.55 * t  # nearer cards first, from the left
        w, h = 430 * scale, 573 * scale
        x = 1880 + 520 * i * (1.0 - 0.32 * t)
        y = base_y - h + 40 * t
        alpha = int(255 * (0.92 - 0.6 * t))
        top, bottom = hex_rgb(hues[i % len(hues)]), hex_rgb(hues[(i + 3) % len(hues)]) * 0.45
        # The card: a vertical gradient, rounded, with a light edge.
        card = Image.new("RGBA", (int(w), int(h)))
        grad = np.linspace(0, 1, int(h), dtype=np.float32)[:, None, None]
        rgb = top * (1 - grad) + bottom * grad
        rgb = np.broadcast_to(rgb, (int(h), int(w), 3))
        a = np.full((int(h), int(w), 1), alpha, dtype=np.float32)
        card = Image.fromarray(np.concatenate([rgb, a], axis=2).astype(np.uint8), "RGBA")
        mask = Image.new("L", card.size, 0)
        ImageDraw.Draw(mask).rounded_rectangle([0, 0, card.size[0] - 1, card.size[1] - 1], radius=int(34 * scale),
                                               fill=alpha)
        layer.paste(card, (int(x), int(y)), mask)
        ld.rounded_rectangle([x, y, x + w, y + h], radius=int(34 * scale), outline=(255, 255, 255, int(alpha * 0.35)),
                             width=max(2, int(4 * scale)))
        gd.rounded_rectangle([x - 30, y - 30, x + w + 30, y + h + 30], radius=int(60 * scale),
                             fill=tuple(int(c) for c in top) + (int(alpha * 0.45),))
        # The focus ring on the first card, as the app draws it.
        if i == 0:
            ld.rounded_rectangle([x - 16, y - 16, x + w + 16, y + h + 16], radius=int(46 * scale),
                                 outline=(255, 255, 255, 240), width=10)
        # A reflection fading into the floor.
        refl = card.transpose(Image.FLIP_TOP_BOTTOM).crop((0, 0, card.size[0], int(h * 0.35)))
        fade = np.linspace(0.22, 0, refl.size[1], dtype=np.float32)[:, None] * alpha
        rmask = Image.fromarray(np.broadcast_to(fade, (refl.size[1], refl.size[0])).astype(np.uint8), "L")
        layer.paste(refl, (int(x), int(y + h + 18)), rmask)
    glow = glow.filter(ImageFilter.GaussianBlur(70))
    canvas.alpha_composite(glow)
    canvas.alpha_composite(layer)
    # The shelf's far end dissolves into the backdrop.
    fade = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ramp = np.clip((np.arange(W, dtype=np.float32) - 3000) / 840, 0, 1) ** 1.5
    alpha = np.broadcast_to((ramp * 200)[None, :], (H, W)).astype(np.uint8)
    fade.putalpha(Image.fromarray(alpha, "L"))
    tint = Image.new("RGBA", (W, H), (11, 16, 38, 255))
    tint.putalpha(Image.fromarray(alpha, "L"))
    canvas.alpha_composite(tint)


def wordmark(canvas):
    draw = ImageDraw.Draw(canvas)
    big = ImageFont.truetype(str(FONT), 300)
    small = ImageFont.truetype(str(FONT), 76)
    x, y = 250, 1560
    # A soft shadow, then the name.
    shadow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(shadow).text((x, y + 18), "RommPS", font=big, fill=(0, 0, 0, 170), anchor="ls")
    canvas.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(26)))
    draw.text((x, y), "RommPS", font=big, fill=(255, 255, 255, 255), anchor="ls")
    draw.rounded_rectangle([x + 8, y + 54, x + 8 + 280, y + 66], radius=6, fill=(118, 214, 255, 255))
    draw.text((x + 8, y + 190), "Your RomM library and save sync", font=small, fill=(255, 255, 255, 190), anchor="ls")


def main():
    out = Path(sys.argv[1])
    canvas = aurora()
    cards(canvas)
    wordmark(canvas)
    canvas.convert("RGB").convert("RGBA").save(out)
    print(f"title art: {out} ({W}x{H})")


if __name__ == "__main__":
    main()
