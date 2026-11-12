#!/usr/bin/env python3
"""Draws the Z-Cancel NX mark: a heavy italic Z cut through the middle with the lower half slid aside,
the landing animation cut short. One flat colour on a dark field; original artwork, no game imagery.

    python3 tools/make_logo.py        -> assets/logo.png (1024, rounded), assets/mark.png (glyph only,
                                         transparent), switch/icon.jpg (256, square, the NRO icon)
Needs Pillow. SPDX-License-Identifier: GPL-3.0-or-later
"""
from pathlib import Path
from PIL import Image, ImageDraw

REPO = Path(__file__).resolve().parent.parent
FIELD = (23, 22, 29)       # near-black with a little violet
MARK = (255, 204, 51)      # warm yellow
SS = 4                     # supersampling


def z_mask(size: int) -> Image.Image:
    """The mark as an 8-bit mask, size x size, drawn at SS times the size and scaled down."""
    n = size * SS
    u = n / 1000.0                      # the design is laid out on a 1000-unit square
    w, h, t, a = 520, 600, 150, 215     # letter width, height, bar thickness, diagonal's horizontal run
    z = [(0, 0), (w, 0), (w, t), (a, h - t), (w, h - t), (w, h), (0, h), (0, h - t), (w - a, t), (0, t)]
    letter = Image.new("L", (n, n), 0)
    ox, oy = (1000 - w) / 2, (1000 - h) / 2
    ImageDraw.Draw(letter).polygon([((ox + x) * u, (oy + y) * u) for x, y in z], fill=255)

    # Cut through the middle; the halves slide apart along the cut.
    gap, slide = 30, 78
    mid = int(500 * u)
    top = letter.crop((0, 0, n, mid - int(gap / 2 * u)))
    bottom = letter.crop((0, mid + int(gap / 2 * u), n, n))
    cut = Image.new("L", (n, n), 0)
    cut.paste(top, (-int(slide / 2 * u), 0))
    cut.paste(bottom, (int(slide / 2 * u), mid + int(gap / 2 * u)))

    # Italic: shear about the centre line (x' = x + k * (y - centre)).
    k = 0.17
    sheared = cut.transform((n, n), Image.AFFINE, (1, k, -k * n / 2, 0, 1, 0), resample=Image.BICUBIC)
    return sheared.resize((size, size), Image.LANCZOS)


def main() -> None:
    (REPO / "assets").mkdir(exist_ok=True)
    (REPO / "switch").mkdir(exist_ok=True)
    size = 1024
    mask = z_mask(size)

    mark = Image.new("RGBA", (size, size), MARK + (0,))
    mark.putalpha(mask)
    mark.save(REPO / "assets" / "mark.png")

    field = Image.new("RGBA", (size, size), FIELD + (255,))
    field.paste(Image.new("RGBA", (size, size), MARK + (255,)), (0, 0), mask)
    corners = Image.new("L", (size * SS, size * SS), 0)
    ImageDraw.Draw(corners).rounded_rectangle((0, 0, size * SS - 1, size * SS - 1), radius=228 * SS, fill=255)
    logo = field.copy()
    logo.putalpha(corners.resize((size, size), Image.LANCZOS))
    logo.save(REPO / "assets" / "logo.png")

    field.convert("RGB").resize((256, 256), Image.LANCZOS).save(REPO / "switch" / "icon.jpg", quality=95)
    print("wrote assets/logo.png, assets/mark.png, switch/icon.jpg")


if __name__ == "__main__":
    main()
