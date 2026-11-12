#!/usr/bin/env python3
"""RommDeck's Steam artwork, from RommPS's title art (platform/ps5/app/tools/
make-title-art.py): the same galaxy, drawn by the same code, and the same
wordmark with "Deck" lit in cyan and violet where RommPS has "PS".

    make-steam-art.py OUT_DIR

Writes what Steam shows for a game:
    capsule.png  600x900    the library grid
    wide.png     920x430    the wide capsule (recent games, shelves)
    hero.png     1920x620   the banner on the game's page (no text: the logo goes over it)
    logo.png     transparent, the wordmark Steam draws over the hero
    icon.png     512x512    lists, and the app's window and menu entry

Needs Pillow and numpy (tools/deck-build.sh has them).

SPDX-License-Identifier: GPL-3.0-or-later
"""
import importlib.util
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location("title_art", REPO / "platform/ps5/app/tools/make-title-art.py")
title = importlib.util.module_from_spec(spec)
spec.loader.exec_module(title)

FIRST, SECOND = "Romm", "Deck"


def backdrop():
    """RommPS's sky at 3840x2160, without its wordmark."""
    rng = np.random.default_rng(11)
    img = title.space(rng)
    img = title.galaxy(rng, img)
    img += rng.normal(0, 1.2, img.shape).astype(np.float32)
    canvas = title.starfield(rng, img)
    title.swirl(rng, canvas)
    return canvas


def crop(sky, aspect, cx, cy, height):
    """The part of the sky around (cx, cy) of the given height and aspect."""
    w = min(title.W, int(round(height * aspect)))
    h = int(round(w / aspect))
    left = int(np.clip(cx - w / 2, 0, title.W - w))
    top = int(np.clip(cy - h / 2, 0, title.H - h))
    return sky.crop((left, top, left + w, top + h))


def darken(img, towards, strength=170, reach=0.8):
    """Darker towards a corner or edge, so the wordmark reads: towards is (u, v), 0..1."""
    w, h = img.size
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    d = np.sqrt(((x / w - towards[0]) * 1.2) ** 2 + (y / h - towards[1]) ** 2)
    a = (np.clip(1 - d / reach, 0, 1) ** 1.5 * strength).astype(np.uint8)
    dark = Image.new("RGBA", (w, h), (2, 3, 10, 255))
    dark.putalpha(Image.fromarray(a, "L"))
    img.alpha_composite(dark)


def wordmark(canvas, x, y, size, anchor="ls", shadow=True):
    """FIRST in white and SECOND filled cyan to violet, with its glow, as RommPS's."""
    w, h = canvas.size
    font = ImageFont.truetype(str(title.FONT), size)
    total = font.getlength(FIRST + SECOND)
    if anchor == "ms":  # centred on x
        x -= total / 2
    first_w = font.getlength(FIRST)
    scale = size / 300  # make-title-art.py draws at 300
    if shadow:
        layer = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        ImageDraw.Draw(layer).text((x, y + 18 * scale), FIRST + SECOND, font=font, fill=(0, 0, 0, 180), anchor="ls")
        canvas.alpha_composite(layer.filter(ImageFilter.GaussianBlur(max(1, 26 * scale))))
    letters_first = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    ImageDraw.Draw(letters_first).text((x, y), FIRST, font=font, fill=(255, 255, 255, 255), anchor="ls")
    mask = Image.new("L", (w, h), 0)
    ImageDraw.Draw(mask).text((x + first_w, y), SECOND, font=font, fill=255, anchor="ls")
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    left, width = x + first_w, font.getlength(SECOND)
    t = np.clip(((xx - left) / width) * 0.75 + ((y - yy) / (220 * scale)) * 0.25, 0, 1)[..., None]
    fill = title.hex_rgb(title.CYAN) * (1 - t) + title.hex_rgb(title.VIOLET) * t
    gradient = Image.fromarray(fill.astype(np.uint8), "RGB").convert("RGBA")
    glow = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    glow.paste(gradient, (0, 0), mask.filter(ImageFilter.MaxFilter(max(3, int(9 * scale) | 1))))
    canvas.alpha_composite(glow.filter(ImageFilter.GaussianBlur(max(1, 34 * scale))))
    canvas.alpha_composite(letters_first)
    letters = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    letters.paste(gradient, (0, 0), mask)
    canvas.alpha_composite(letters)


def fit_size(width, start=300):
    """The largest font size at which the wordmark is at most width wide."""
    size = start
    while size > 8 and ImageFont.truetype(str(title.FONT), size).getlength(FIRST + SECOND) > width:
        size -= 2
    return size


def main():
    global FIRST, SECOND
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    sky = backdrop()
    gx, gy = 0.68 * title.W, 0.43 * title.H  # the galaxy's centre (make-title-art.py)

    capsule = crop(sky, 600 / 900, gx - 120, title.H / 2, title.H).resize((600, 900), Image.LANCZOS)
    darken(capsule, (0.5, 1.0), 200, 0.75)
    wordmark(capsule, 300, 820, fit_size(520), anchor="ms")
    capsule.convert("RGB").save(out / "capsule.png")

    wide = crop(sky, 920 / 430, title.W / 2, gy + 120, title.H).resize((920, 430), Image.LANCZOS)
    darken(wide, (0.0, 1.0))
    wordmark(wide, 44, 380, fit_size(430))
    wide.convert("RGB").save(out / "wide.png")

    hero = crop(sky, 1920 / 620, title.W / 2, gy + 40, title.H).resize((1920, 620), Image.LANCZOS)
    hero.convert("RGB").save(out / "hero.png")

    size = 240
    font = ImageFont.truetype(str(title.FONT), size)
    pad = 80
    lw, lh = int(font.getlength(FIRST + SECOND)) + 2 * pad, int(size * 1.1) + 2 * pad
    logo = Image.new("RGBA", (lw, lh), (0, 0, 0, 0))
    wordmark(logo, pad, pad + int(size * 0.85), size, shadow=False)
    logo.save(out / "logo.png")

    icon = crop(sky, 1.0, gx, gy, 1100).resize((512, 512), Image.LANCZOS)
    darken(icon, (0.5, 1.0), 150, 0.9)
    # The icon carries just the initials, R and D, the same way.
    FIRST, SECOND = "R", "D"
    wordmark(icon, 256, 360, 250, anchor="ms")
    FIRST, SECOND = "Romm", "Deck"
    icon.convert("RGB").save(out / "icon.png")
    print(f"Steam artwork in {out}")


if __name__ == "__main__":
    main()
