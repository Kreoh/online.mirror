#!/usr/bin/env python3
#
# This file is part of the Collabora Office project.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.
#
# Generates the drag-to-Applications background for the macOS .dmg, at 1x and
# 2x, in two variants:
#
#   dmg-background.png          titled with the product name
#   dmg-background-generic.png  no product name, for rebranded builds
#
# macos/make-dmg.sh picks the variant that matches the name of the bundle it is
# packaging, and hands the 1x PNG to dmgbuild, which finds the @2x sibling by
# name and compiles the two into the multi-resolution TIFF that Finder wants.
# Rebranded builds that do want their own name in the title can regenerate the
# titled variant with --name.
#
# The geometry constants below are also written out to geometry.env, which
# make-dmg.sh sources for its create-dmg arguments, so the window size and the
# icon positions only ever get defined here.

import argparse
import math
import os

from PIL import Image, ImageDraw, ImageFont

DEFAULT_NAME = "Collabora Office"

# Window content area, in points. Passed to create-dmg as --window-size.
WIDTH = 680
HEIGHT = 400

# Icon centres, in points. Passed to create-dmg as --icon and --app-drop-link.
ICON_SIZE = 128
APP_CENTRE = (170, 190)
DROP_CENTRE = (510, 190)

# Colours
BG_TOP = (250, 250, 252)
BG_BOTTOM = (236, 236, 241)
ARROW = (150, 150, 158)
TITLE_COLOUR = (29, 29, 31)
HINT_COLOUR = (110, 110, 115)

HINT = "Drag the app icon onto the Applications folder"

FONT_DIRS = [
    "/usr/share/fonts/truetype/lato",
    "/System/Library/Fonts",
    "/usr/share/fonts/truetype/liberation2",
    "/usr/share/fonts/truetype/msttcorefonts",
]
TITLE_FONTS = ["Lato-Bold.ttf", "HelveticaNeue.ttc", "LiberationSans-Bold.ttf", "Arial_Bold.ttf"]
HINT_FONTS = ["Lato-Regular.ttf", "Helvetica.ttc", "LiberationSans-Regular.ttf", "Arial.ttf"]

# Supersampling factor for the vector parts; text is drawn at final size.
SS = 4


def load_font(candidates, size):
    for name in candidates:
        for directory in FONT_DIRS:
            path = os.path.join(directory, name)
            if os.path.exists(path):
                return ImageFont.truetype(path, size)
    return ImageFont.load_default()


def gradient(width, height):
    image = Image.new("RGB", (width, height))
    draw = ImageDraw.Draw(image)
    for y in range(height):
        t = y / max(1, height - 1)
        draw.line(
            [(0, y), (width, y)],
            fill=tuple(round(a + (b - a) * t) for a, b in zip(BG_TOP, BG_BOTTOM)),
        )
    return image


def bezier(p0, p1, p2, steps=160):
    points = []
    for i in range(steps + 1):
        t = i / steps
        u = 1 - t
        points.append(
            (
                u * u * p0[0] + 2 * u * t * p1[0] + t * t * p2[0],
                u * u * p0[1] + 2 * u * t * p1[1] + t * t * p2[1],
            )
        )
    return points


def draw_arrow(image, scale):
    """Curved arrow from the app icon towards the Applications folder."""
    layer = Image.new("RGBA", (image.width * SS, image.height * SS), (0, 0, 0, 0))
    draw = ImageDraw.Draw(layer)
    s = scale * SS

    start_x = APP_CENTRE[0] + ICON_SIZE / 2 + 24
    end_x = DROP_CENTRE[0] - ICON_SIZE / 2 - 22
    y = APP_CENTRE[1]
    head = 26

    p0 = (start_x * s, y * s)
    p2 = ((end_x - head * 0.55) * s, y * s)
    p1 = ((start_x + end_x) / 2 * s, (y - 42) * s)

    points = bezier(p0, p1, p2)
    draw.line(points, fill=ARROW + (255,), width=round(7 * s), joint="curve")
    # Round off the tail.
    r = 3.5 * s
    draw.ellipse([p0[0] - r, p0[1] - r, p0[0] + r, p0[1] + r], fill=ARROW + (255,))

    # Arrow head, aligned with the tangent at the end of the curve.
    tail, tip = points[-12], points[-1]
    angle = math.atan2(tip[1] - tail[1], tip[0] - tail[0])
    length = head * s
    half = head * 0.62 * s
    apex = (tip[0] + math.cos(angle) * length, tip[1] + math.sin(angle) * length)
    left = (
        tip[0] + math.cos(angle + math.pi / 2) * half,
        tip[1] + math.sin(angle + math.pi / 2) * half,
    )
    right = (
        tip[0] + math.cos(angle - math.pi / 2) * half,
        tip[1] + math.sin(angle - math.pi / 2) * half,
    )
    draw.polygon([apex, left, right], fill=ARROW + (255,))

    layer = layer.resize(image.size, Image.LANCZOS)
    image.paste(layer, (0, 0), layer)


def draw_text(image, scale, title):
    draw = ImageDraw.Draw(image)
    if title:
        draw.text(
            (WIDTH / 2 * scale, 50 * scale),
            title,
            font=load_font(TITLE_FONTS, round(22 * scale)),
            fill=TITLE_COLOUR,
            anchor="mm",
        )
    draw.text(
        (WIDTH / 2 * scale, 330 * scale),
        HINT,
        font=load_font(HINT_FONTS, round(13 * scale)),
        fill=HINT_COLOUR,
        anchor="mm",
    )


def render(scale, title):
    image = gradient(WIDTH * scale, HEIGHT * scale)
    draw_arrow(image, scale)
    draw_text(image, scale, title)
    return image


def write_variant(outdir, stem, title):
    for scale, suffix in ((1, ""), (2, "@2x")):
        path = os.path.join(outdir, "%s%s.png" % (stem, suffix))
        render(scale, title).save(path)
        print("wrote %s (%dx%d)" % (path, WIDTH * scale, HEIGHT * scale))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--name",
        default=DEFAULT_NAME,
        help="product name for the title of the titled variant (default: %s)" % DEFAULT_NAME,
    )
    parser.add_argument("--outdir", default=os.path.dirname(os.path.abspath(__file__)))
    args = parser.parse_args()

    write_variant(args.outdir, "dmg-background", "Install %s" % args.name)
    write_variant(args.outdir, "dmg-background-generic", None)

    path = os.path.join(args.outdir, "geometry.env")
    with open(path, "w") as env:
        env.write("# Generated by make-dmg-background.py -- do not edit.\n")
        env.write("DMG_WINDOW_WIDTH=%d\n" % WIDTH)
        env.write("DMG_WINDOW_HEIGHT=%d\n" % HEIGHT)
        env.write("DMG_ICON_SIZE=%d\n" % ICON_SIZE)
        env.write("DMG_APP_X=%d\n" % APP_CENTRE[0])
        env.write("DMG_APP_Y=%d\n" % APP_CENTRE[1])
        env.write("DMG_DROP_X=%d\n" % DROP_CENTRE[0])
        env.write("DMG_DROP_Y=%d\n" % DROP_CENTRE[1])
        # The name baked into the titled variant, so that make-dmg.sh can tell
        # whether that variant applies to the bundle it is packaging.
        env.write("DMG_TITLE_NAME='%s'\n" % args.name.replace("'", "'\\''"))
    print("wrote %s" % path)


if __name__ == "__main__":
    main()
