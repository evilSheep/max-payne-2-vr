#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Builds Steam library artwork for a "Max Payne 2 VR" entry from Max Payne 2's own art.

usage: make_art.py <steam-art-dir> <out-dir> [font.ttf]
<steam-art-dir> holds Steam's cached art for app 12150 (library_hero.jpg, logo.png), found on
the Frame in ~/.local/share/Steam/appcache/librarycache/12150. The output is derived from the
game's artwork, so keep it local: it is for your own library, not for publishing.
Writes capsule.png (600x900), header.png (920x430), hero.png (1920x620), logo.png and
icon.png (256x256). Needs Pillow.
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

src, out = sys.argv[1], sys.argv[2]
font_path = sys.argv[3] if len(sys.argv) > 3 else "/System/Library/Fonts/Supplemental/DIN Condensed Bold.ttf"
os.makedirs(out, exist_ok=True)

RED = (220, 32, 39, 255)      # the logo's red
INK = (17, 17, 17, 255)       # the logo's outline
WHITE = (255, 255, 255, 255)

hero = Image.open(os.path.join(src, "library_hero.jpg")).convert("RGBA")
logo_full = Image.open(os.path.join(src, "logo.png")).convert("RGBA")
logo = logo_full.crop(logo_full.getbbox())

# the silhouette in the hero art, found by its dark pixels
grey = hero.convert("L")
w, h = grey.size
xs = [x for x in range(w) if min(grey.getpixel((x, y)) for y in range(0, h, 3)) < 128]
ys = [y for y in range(h) if min(grey.getpixel((x, y)) for x in range(0, w, 3)) < 128]
figure = hero.crop((xs[0], ys[0], xs[-1] + 1, ys[-1] + 1))


def vr_tag(height):
    """A red tag reading VR, outlined like the logo's lettering."""
    font = ImageFont.truetype(font_path, int(height * 0.98))
    box = font.getbbox("VR")
    tw, th = box[2] - box[0], box[3] - box[1]
    pad_x, border = int(height * 0.28), max(2, height // 18)
    tag = Image.new("RGBA", (tw + 2 * pad_x, height), (0, 0, 0, 0))
    d = ImageDraw.Draw(tag)
    d.rectangle((0, 0, tag.width - 1, height - 1), fill=INK)
    d.rectangle((border, border, tag.width - 1 - border, height - 1 - border), fill=RED)
    d.text(((tag.width - tw) // 2 - box[0], (height - th) // 2 - box[1]), "VR", font=font, fill=WHITE)
    return tag


def scaled(img, width=None, height=None):
    if width:
        height = round(img.height * width / img.width)
    else:
        width = round(img.width * height / img.height)
    return img.resize((width, height), Image.LANCZOS)


def logo_with_tag(width):
    """The game's logo with the VR tag set under its right-hand end."""
    lg = scaled(logo, width=width)
    tag = vr_tag(round(lg.height * 0.36))
    gap = round(lg.height * 0.10)
    canvas = Image.new("RGBA", (lg.width, lg.height + gap + tag.height), (0, 0, 0, 0))
    canvas.alpha_composite(lg, (0, 0))
    canvas.alpha_composite(tag, (lg.width - tag.width, lg.height + gap))
    return canvas


def save(img, name):
    img.save(os.path.join(out, name))
    print(name, img.size)


# capsule: logo and tag over the silhouette, on the cover's white
cap = Image.new("RGBA", (600, 900), WHITE)
lt = logo_with_tag(552)
cap.alpha_composite(lt, ((600 - lt.width) // 2, 34))
fig = scaled(figure, height=900 - (34 + lt.height + 24))
cap.alpha_composite(fig, ((600 - fig.width) // 2, 900 - fig.height))
save(cap.convert("RGB"), "capsule.png")

# header: logo and tag on the left, silhouette on the right
hdr = Image.new("RGBA", (920, 430), WHITE)
lt = logo_with_tag(500)
hdr.alpha_composite(lt, (40, (430 - lt.height) // 2))
fig = scaled(figure, height=415)
hdr.alpha_composite(fig, (920 - fig.width - 60, 430 - fig.height))
save(hdr.convert("RGB"), "header.png")

# hero: the game's own; Steam lays the logo over its empty left side
save(hero.convert("RGB"), "hero.png")

# logo: the game's logo with the tag
save(logo_with_tag(1280), "logo.png")

# icon: the two faces, with the tag in the corner
icon = Image.new("RGBA", (256, 256), WHITE)
fx, fy = xs[0] + 70, ys[0] - 10
faces = hero.crop((fx, fy, fx + 330, fy + 330)).resize((256, 256), Image.LANCZOS)
icon.alpha_composite(faces, (0, 0))
tag = vr_tag(78)
icon.alpha_composite(tag, (256 - tag.width - 6, 256 - tag.height - 6))
save(icon.convert("RGB"), "icon.png")
