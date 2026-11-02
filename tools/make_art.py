#!/usr/bin/env python3
"""Converts a picture into select screen art for the offline mod's extra fighters.

    python3 tools/make_art.py <picture> <Cyrax|Sektor|Kronika> <cell|large> [--focus X,Y] [--out DIR]

cell:  the grid portrait, 128 x 256 (the cell frame shows about the top 200 rows).
large: the versus art beside the grid, 1024 x 1088 (about the left 650 columns show beside the grid, so the
       face belongs in the left half). Player 2 gets a mirrored copy, which needs 256 more columns to reach
       the screen edge: the file carries them on the left as a dark, blurred reflection of the picture's
       left edge (player 1 skips them).

The picture is scaled to cover the target and cropped around --focus (fractions of the picture's width and
height, default 0.5,0.3: centred, nearer the top, where a face usually is). The output is
<DIR>/<Name>_<cell|large>.art (default DIR: ./art); copy it to sdmc:/mk11offline/art/ on the SD card.

Format: "MK11ART2", width, height and left padding as little-endian u32, then width * height RGBA pixels,
row by row.
Needs Pillow (pip install Pillow).
"""
import argparse
import os
import struct
import sys

from PIL import Image, ImageFilter

SIZES = {"cell": (128, 256), "large": (1024, 1088)}
PAD = {"cell": 0, "large": 256}  # columns added on the left
NAMES = ("Cyrax", "Sektor", "Kronika")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("picture")
    ap.add_argument("name", choices=NAMES)
    ap.add_argument("kind", choices=SIZES)
    ap.add_argument("--focus", default="0.5,0.3")
    ap.add_argument("--out", default="art")
    a = ap.parse_args()

    fx, fy = (float(v) for v in a.focus.split(","))
    tw, th = SIZES[a.kind]
    img = Image.open(a.picture).convert("RGBA")
    scale = max(tw / img.width, th / img.height)
    w, h = round(img.width * scale), round(img.height * scale)
    img = img.resize((w, h), Image.LANCZOS)
    x = min(max(round(fx * w - tw / 2), 0), w - tw)
    y = min(max(round(fy * h - th / 2), 0), h - th)
    img = img.crop((x, y, x + tw, y + th))

    pad = PAD[a.kind]
    if pad:
        # The picture's left edge mirrored outwards, blurred and faded to black.
        edge = img.crop((0, 0, pad, th)).transpose(Image.FLIP_LEFT_RIGHT).filter(ImageFilter.GaussianBlur(12))
        px = edge.load()
        for x in range(pad):
            k = (x + 1) / pad * 0.6  # darker further from the picture
            for y in range(th):
                r, g, b, _ = px[x, y]
                px[x, y] = (int(r * k), int(g * k), int(b * k), 255)
        wide = Image.new("RGBA", (tw + pad, th))
        wide.paste(edge, (0, 0))
        wide.paste(img, (pad, 0))
        img = wide
        tw += pad

    os.makedirs(a.out, exist_ok=True)
    path = os.path.join(a.out, f"{a.name}_{a.kind}.art")
    with open(path, "wb") as f:
        f.write(b"MK11ART2" + struct.pack("<III", tw, th, pad) + img.tobytes())
    img.convert("RGB").save(os.path.splitext(path)[0] + "_preview.png")
    print(f"{path} ({tw} x {th}), preview {os.path.splitext(path)[0]}_preview.png")


if __name__ == "__main__":
    sys.exit(main())
