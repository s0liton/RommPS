#!/usr/bin/env python3
"""RommPS's dashboard background, drawn from nothing but code.

    make-title-art.py OUT.png

A 3840x2160 picture for sce_sys/pic0.dds and pic1.dds (tools/title-art.sh
turns it into BC7): deep space with soft nebulae, a spiral galaxy swirling on
the right, a field of stars, and the RommPS wordmark in Montserrat (OFL,
third_party/Montserrat-Medium.ttf), its "PS" lit in the app's cyan and violet.
Every shape is drawn here.

SPDX-License-Identifier: GPL-3.0-or-later
"""
import math
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

W, H = 3840, 2160
REPO = Path(__file__).resolve().parents[4]
FONT = REPO / "third_party/Montserrat-Medium.ttf"
CYAN, VIOLET, BLUE = 0x76d6ff, 0xc04bd6, 0x3a5bd9


def hex_rgb(value):
    return np.array([(value >> 16) & 255, (value >> 8) & 255, value & 255], dtype=np.float32)


def blur(field, radius):
    """A float field blurred, through Pillow (values kept in 0..1)."""
    img = Image.fromarray(np.clip(field * 255, 0, 255).astype(np.uint8), "L")
    return np.asarray(img.filter(ImageFilter.GaussianBlur(radius)), dtype=np.float32) / 255


def noise(rng, cells, radius):
    """Smooth clouds: random cells scaled up and softened."""
    small = Image.fromarray((rng.random((cells[1], cells[0])) * 255).astype(np.uint8), "L")
    big = small.resize((W, H), Image.BICUBIC).filter(ImageFilter.GaussianBlur(radius))
    return np.asarray(big, dtype=np.float32) / 255


def space(rng):
    """Deep navy, with nebulae in the app's blue, violet and cyan."""
    y, x = np.mgrid[0:H, 0:W].astype(np.float32)
    u, v = x / W, y / H
    img = hex_rgb(0x03050d) * (1 - v[..., None]) + hex_rgb(0x090c20) * v[..., None]
    clouds = 0.55 * noise(rng, (8, 5), 260) + 0.30 * noise(rng, (16, 9), 140) + 0.15 * noise(rng, (32, 18), 70)
    clouds = np.clip((clouds - 0.40) * 2.6, 0, 1) ** 1.8
    # Brighter towards the galaxy, quiet behind the wordmark.
    near = np.exp(-(((u - 0.70) * W / H) ** 2 + (v - 0.42) ** 2) / 0.35)
    for colour, k, shift in [(BLUE, 0.55, 0.0), (VIOLET, 0.40, 0.25), (CYAN, 0.18, 0.5)]:
        tint = np.roll(clouds, int(shift * W / 3), axis=1) * (0.25 + 0.75 * near)
        img = img + hex_rgb(colour) * (tint * k)[..., None]
    return img


def galaxy(rng, img):
    """A two-armed spiral seen at a slant, its arms trailing into the swirl."""
    cx, cy, tilt, squash = 0.68 * W, 0.43 * H, math.radians(-22), 0.52
    stars = np.zeros((H, W), dtype=np.float32)
    colour = np.zeros((H, W, 3), dtype=np.float32)

    def place(r, theta, bright, rgb):
        # The disc: a circle squashed into an ellipse, then tilted.
        dx, dy = r * np.cos(theta), r * np.sin(theta) * squash
        x = cx + dx * math.cos(tilt) - dy * math.sin(tilt)
        y = cy + dx * math.sin(tilt) + dy * math.cos(tilt)
        keep = (x >= 0) & (x < W) & (y >= 0) & (y < H)
        xi, yi = x[keep].astype(int), y[keep].astype(int)
        np.add.at(stars, (yi, xi), bright[keep])
        np.add.at(colour, (yi, xi), rgb[None, :] * bright[keep, None])

    n = 160000
    for arm in range(2):
        t = rng.random(n) ** 0.8 * 4.2 * math.pi            # how far round the arm
        r = 95 * np.exp(0.185 * t)                           # a logarithmic spiral
        spread = 0.07 + 0.05 * t / (4.2 * math.pi)
        theta = t + arm * math.pi + rng.normal(0, spread, n) * (1 + t / 8)
        r = r * (1 + rng.normal(0, 0.10, n))
        bright = (0.15 + rng.random(n) ** 3) * (1.3 - 0.8 * t / (4.2 * math.pi))
        place(r, theta, bright, hex_rgb(0xcfdcff))
        # The outer arms take the app's colours.
        far = t > 2.2 * math.pi
        place(r[far] * 1.01, theta[far] + 0.02, bright[far] * 0.6, hex_rgb(CYAN if arm else VIOLET))
    # The bulge: a dense, warm core.
    m = 40000
    r = np.abs(rng.normal(0, 150, m))
    place(r, rng.random(m) * 2 * math.pi, rng.random(m) ** 2 * 0.5, hex_rgb(0xffe6c4))

    norm = np.maximum(stars, 1e-6)[..., None]
    tint = colour / norm
    sharp = np.clip(stars, 0, 1)
    haze = blur(np.clip(stars * 0.08, 0, 1), 40) * 6 + blur(np.clip(stars * 0.2, 0, 1), 10) * 2.5
    glow_tint = np.stack([blur(np.clip(tint[..., c] / 255 * np.clip(stars, 0, 1), 0, 1), 30) for c in range(3)], -1)
    glow_tint = glow_tint / np.maximum(blur(sharp, 30), 1e-3)[..., None] * 255
    img = img + glow_tint * np.clip(haze, 0, 1.5)[..., None] * 0.55
    img = img + tint * sharp[..., None] * 0.9
    # The core's light, falling off over the whole disc.
    y, x = np.mgrid[0:H, 0:W].astype(np.float32)
    dx, dy = x - cx, y - cy
    rx = dx * math.cos(-tilt) - dy * math.sin(-tilt)
    ry = (dx * math.sin(-tilt) + dy * math.cos(-tilt)) / squash
    d = np.sqrt(rx * rx + ry * ry)
    img = img + hex_rgb(0xfff1dc) * (np.exp(-d / 45) * 0.75 + np.exp(-d / 220) * 0.28)[..., None]
    img = img + hex_rgb(BLUE) * (np.exp(-d / 1100) * 0.22)[..., None]
    return img


def starfield(rng, img):
    """Stars across the sky; a few bright ones with a soft cross."""
    n = 14000
    x, y = rng.integers(0, W, n), rng.integers(0, H, n)
    bright = 0.12 + 0.88 * rng.random(n) ** 5
    field = np.zeros((H, W), dtype=np.float32)
    np.add.at(field, (y, x), bright * 1.4)
    img = img + 255 * np.clip(field, 0, 1)[..., None] + 255 * blur(np.clip(field * 0.5, 0, 1), 2)[..., None] * 2
    layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    draw = ImageDraw.Draw(layer)
    for _ in range(26):
        sx, sy, s = rng.integers(0, W), rng.integers(0, H), rng.uniform(10, 34)
        rgb = tuple(int(c) for c in [hex_rgb(0xffffff), hex_rgb(CYAN), hex_rgb(0xffe6c4)][rng.integers(0, 3)])
        draw.line([(sx - s, sy), (sx + s, sy)], fill=rgb + (150,), width=2)
        draw.line([(sx, sy - s), (sx, sy + s)], fill=rgb + (150,), width=2)
        draw.ellipse([sx - 4, sy - 4, sx + 4, sy + 4], fill=rgb + (255,))
    glow = layer.filter(ImageFilter.GaussianBlur(5))
    out = Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), "RGB").convert("RGBA")
    out.alpha_composite(glow)
    out.alpha_composite(layer)
    return out


def swirl(rng, canvas):
    """Faint star trails arcing round the galaxy, as on a long exposure."""
    cx, cy = 0.68 * W, 0.43 * H
    layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    draw = ImageDraw.Draw(layer)
    for _ in range(420):
        r = rng.uniform(600, 2600)
        start = rng.uniform(0, 360)
        length = rng.uniform(4, 14) * (1100 / r)  # degrees: inner trails sweep further
        rgb = [hex_rgb(0xffffff), hex_rgb(CYAN), hex_rgb(VIOLET), hex_rgb(0xcfdcff)][rng.integers(0, 4)]
        alpha = int(rng.uniform(25, 90) * min(1, 900 / r + 0.3))
        box = [cx - r, cy - r * 0.62, cx + r, cy + r * 0.62]
        draw.arc(box, start, start + length, fill=tuple(int(c) for c in rgb) + (alpha,), width=2)
    canvas.alpha_composite(layer.filter(ImageFilter.GaussianBlur(1.2)))


def shade(canvas):
    """Darker towards the lower left, so the wordmark reads."""
    y, x = np.mgrid[0:H, 0:W].astype(np.float32)
    d = np.sqrt(((x / W - 0.0) * 1.2) ** 2 + ((y / H - 1.0) * 1.0) ** 2)
    a = (np.clip(1 - d / 0.75, 0, 1) ** 1.5 * 150).astype(np.uint8)
    dark = Image.new("RGBA", (W, H), (2, 3, 10, 255))
    dark.putalpha(Image.fromarray(a, "L"))
    canvas.alpha_composite(dark)


def wordmark(canvas):
    """"Romm" in white, "PS" filled cyan to violet, with a glow of its own."""
    font = ImageFont.truetype(str(FONT), 300)
    x, y = 250, 1700
    romm_w = font.getlength("Romm")
    shadow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(shadow).text((x, y + 18), "RommPS", font=font, fill=(0, 0, 0, 180), anchor="ls")
    canvas.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(26)))
    ImageDraw.Draw(canvas).text((x, y), "Romm", font=font, fill=(255, 255, 255, 255), anchor="ls")

    # The letters' shape, filled with a diagonal gradient.
    mask = Image.new("L", (W, H), 0)
    ImageDraw.Draw(mask).text((x + romm_w, y), "PS", font=font, fill=255, anchor="ls")
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
    left, width = x + romm_w, font.getlength("PS")
    t = np.clip(((xx - left) / width) * 0.75 + ((y - yy) / 220) * 0.25, 0, 1)[..., None]
    fill = hex_rgb(CYAN) * (1 - t) + hex_rgb(VIOLET) * t
    gradient = Image.fromarray(fill.astype(np.uint8), "RGB").convert("RGBA")
    glow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    glow.paste(gradient, (0, 0), mask.filter(ImageFilter.MaxFilter(9)))
    canvas.alpha_composite(glow.filter(ImageFilter.GaussianBlur(34)))
    letters = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    letters.paste(gradient, (0, 0), mask)
    canvas.alpha_composite(letters)


def main():
    out = Path(sys.argv[1])
    rng = np.random.default_rng(11)
    img = space(rng)
    img = galaxy(rng, img)
    img += rng.normal(0, 1.2, img.shape).astype(np.float32)  # grain, so gradients never band
    canvas = starfield(rng, img)
    swirl(rng, canvas)
    shade(canvas)
    wordmark(canvas)
    canvas.convert("RGB").convert("RGBA").save(out)
    print(f"title art: {out} ({W}x{H})")


if __name__ == "__main__":
    main()
