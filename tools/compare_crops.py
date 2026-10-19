"""Compose a labeled comparison sheet from add-on screenshots: one column per shot, one row per crop, pixels 1:1.

usage: compare_crops.py <shots dir> <out.jpg> --col <name> <title> <subtitle> [--col ...] --crop x y w h
                        [--crop ...] [--pq] [--quality N]
Columns are shots (shots/<name>.bin + .txt, as tools/shots.py reads them); rows are crop rectangles in the shots'
pixels, all the same size. --pq decodes HDR captures the add-on tagged as sRGB (see shots.load). Prints the
sharpness and fine-noise figures of tools/shots.py per tile.
"""
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shots  # noqa: E402

GAP, HEADER = 6, 64
BACKGROUND, TITLE, SUBTITLE = (17, 17, 19), (255, 255, 255), (200, 200, 204)


def font(name, size):
    try:
        return ImageFont.truetype(os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "Fonts", name), size)
    except OSError:
        return ImageFont.load_default()


def main():
    args = sys.argv[1:]
    shots_dir, out = args[0], args[1]
    cols, crops, space, quality = [], [], None, 90
    i = 2
    while i < len(args):
        if args[i] == "--col":
            cols.append(tuple(args[i + 1:i + 4]))
            i += 4
        elif args[i] == "--crop":
            crops.append(tuple(int(v) for v in args[i + 1:i + 5]))
            i += 5
        elif args[i] == "--pq":
            space = shots.HDR10_PQ
            i += 1
        elif args[i] == "--quality":
            quality = int(args[i + 1])
            i += 2
        else:
            sys.exit("unknown argument " + args[i])
    if not cols or not crops or len({(w, h) for _, _, w, h in crops}) != 1:
        sys.exit(__doc__)
    cw, ch = crops[0][2:]
    sheet = Image.new("RGB", (len(cols) * cw + (len(cols) - 1) * GAP, HEADER + len(crops) * ch + (len(crops) - 1) * GAP),
                      BACKGROUND)
    draw = ImageDraw.Draw(sheet)
    bold, regular = font("segoeuib.ttf", 22), font("segoeui.ttf", 17)
    for c, (name, title, subtitle) in enumerate(cols):
        img = shots.load(shots_dir, name, space)
        x0 = c * (cw + GAP)
        draw.text((x0 + 12, 8), title, font=bold, fill=TITLE)
        draw.text((x0 + 12, 37), subtitle, font=regular, fill=SUBTITLE)
        for r, (x, y, w, h) in enumerate(crops):
            part = img[y:y + h, x:x + w]
            sharp, noise = shots.metrics(part)
            print("%-16s crop %d: sharpness %6.2f  fine noise %7.2f  brightness %4.1f" % (
                name, r, sharp, noise, part.mean() * 100))
            sheet.paste(Image.fromarray((part * 255 + 0.5).astype(np.uint8)), (x0, HEADER + r * (ch + GAP)))
    sheet.save(out, quality=quality, subsampling=0)
    print("wrote", out, sheet.size)


if __name__ == "__main__":
    main()
