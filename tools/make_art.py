#!/usr/bin/env python3
"""Converts a picture into select screen art for the offline mod's extra fighters.

    python3 tools/make_art.py <picture> <Cyrax|Sektor|Kronika> <cell|large> [--focus X,Y] [--out DIR]

cell:  the grid portrait, 128 x 256 (the cell frame shows about the top 200 rows).
large: the versus art beside the grid, 1024 x 1088. Player 1 shows about the left 650 columns, player 2 (a
       mirrored copy) about the middle 650, so the face belongs near the middle.

The picture is scaled to cover the target and cropped around --focus (fractions of the picture's width and
height, default 0.5,0.3: centred, nearer the top, where a face usually is). The output is
<DIR>/<Name>_<cell|large>.art (default DIR: ./art); copy it to sdmc:/mk11offline/art/ on the SD card.

Format: "MK11ART2", width, height and a count of left columns to leave out (0 here) as little-endian u32, then
width * height RGBA pixels, row by row.
Needs Pillow (pip install Pillow).
"""
import argparse
import os
import struct
import sys

from PIL import Image

SIZES = {"cell": (128, 256), "large": (1024, 1088)}
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

    pad = 0  # columns to leave out on the left (kept in the format)

    os.makedirs(a.out, exist_ok=True)
    path = os.path.join(a.out, f"{a.name}_{a.kind}.art")
    with open(path, "wb") as f:
        f.write(b"MK11ART2" + struct.pack("<III", tw, th, pad) + img.tobytes())
    img.convert("RGB").save(os.path.splitext(path)[0] + "_preview.png")
    print(f"{path} ({tw} x {th}), preview {os.path.splitext(path)[0]}_preview.png")


if __name__ == "__main__":
    sys.exit(main())
