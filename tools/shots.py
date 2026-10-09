"""Turn raw add-on screenshots (shots/<name>.bin + .txt) into viewable PNGs and side-by-side crops.

usage: shots.py <shots dir> <out dir> <name> [<name> ...] [--crop x y w h] [--scale N]
HDR10 (PQ) captures are converted to an SDR preview with a fixed exposure, so all variants are comparable.
Prints a sharpness (mean gradient) and fine-noise (Laplacian energy) figure per crop, plus RMS detail per octave
(1-8 px feature sizes) and mean brightness, to tell lost texture apart from removed pixel noise.
"""
import os
import sys

import numpy as np
from PIL import Image, ImageDraw

R10G10B10A2, R8G8B8A8, RGBA16F = 24, 28, 10
SRGB, SCRGB, HDR10_PQ = 1, 2, 3


def load(shots, name):
    w, h, fmt, space = (int(x) for x in open(os.path.join(shots, name + ".txt")).read().split())
    raw = np.fromfile(os.path.join(shots, name + ".bin"), dtype=np.uint32 if fmt != RGBA16F else np.float16)
    if fmt == R10G10B10A2:
        v = raw.reshape(h, w)
        rgb = np.stack([(v & 0x3FF), (v >> 10) & 0x3FF, (v >> 20) & 0x3FF], -1).astype(np.float32) / 1023.0
    elif fmt == R8G8B8A8:
        rgb = raw.view(np.uint8).reshape(h, w, 4)[..., :3].astype(np.float32) / 255.0
    else:
        rgb = raw.reshape(h, w, 4)[..., :3].astype(np.float32)
    return to_sdr(rgb, space)


def to_sdr(rgb, space):
    if space == HDR10_PQ:
        m1, m2, c1, c2, c3 = 0.1593017578125, 78.84375, 0.8359375, 18.8515625, 18.6875
        p = np.power(np.clip(rgb, 0, 1), 1 / m2)
        nits = 10000 * np.power(np.maximum(p - c1, 0) / (c2 - c3 * p), 1 / m1)
        bt2020_to_709 = np.array([[1.6605, -0.5876, -0.0728], [-0.1246, 1.1329, -0.0083], [-0.0182, -0.1006, 1.1187]])
        lin = np.clip(nits @ bt2020_to_709.T, 0, None) / 203.0  # 203 nits = SDR reference white
    elif space == SCRGB:
        lin = np.clip(rgb, 0, None) * 80.0 / 203.0
    else:
        return np.clip(rgb, 0, 1)
    lin = lin / (1.0 + lin / 4.0)  # gentle roll-off for highlights
    return np.clip(np.where(lin <= 0.0031308, lin * 12.92, 1.055 * np.power(lin, 1 / 2.4) - 0.055), 0, 1)


def metrics(img):
    y = img @ np.array([0.2126, 0.7152, 0.0722])
    gx, gy = np.diff(y, axis=1)[:-1, :], np.diff(y, axis=0)[:, :-1]
    lap = 4 * y[1:-1, 1:-1] - y[:-2, 1:-1] - y[2:, 1:-1] - y[1:-1, :-2] - y[1:-1, 2:]
    return float(np.mean(np.hypot(gx, gy))) * 1000, float(np.mean(lap ** 2)) * 1e5


def octaves(img, levels=4):
    """RMS detail (x1000) at 1, 2, 4, 8 px feature sizes: the difference between each level and its 2x2 average."""
    y = img @ np.array([0.2126, 0.7152, 0.0722])
    out = []
    for _ in range(levels):
        y = y[:y.shape[0] // 2 * 2, :y.shape[1] // 2 * 2]
        small = 0.25 * (y[0::2, 0::2] + y[1::2, 0::2] + y[0::2, 1::2] + y[1::2, 1::2])
        out.append(float(np.sqrt(np.mean((y - np.repeat(np.repeat(small, 2, 0), 2, 1)) ** 2))) * 1000)
        y = small
    return out


def main():
    args = sys.argv[1:]
    crop = None
    if "--crop" in args:
        i = args.index("--crop")
        crop = tuple(int(v) for v in args[i + 1:i + 5])
        args = args[:i] + args[i + 5:]
    scale = 2
    if "--scale" in args:
        i = args.index("--scale")
        scale = int(args[i + 1])
        args = args[:i] + args[i + 2:]
    shots, out, names = args[0], args[1], args[2:]
    os.makedirs(out, exist_ok=True)
    tiles = []
    for name in names:
        img = load(shots, name)
        h, w = img.shape[:2]
        Image.fromarray((img * 255).astype(np.uint8)).resize((w // 3, h // 3), Image.LANCZOS).save(
            os.path.join(out, name + "_full.png"))
        x, y, cw, ch = crop or (w // 2 - 240, h * 2 // 3 - 135, 480, 270)
        part = img[y:y + ch, x:x + cw]
        sharp, noise = metrics(part)
        detail = " ".join("%5.1f" % v for v in octaves(part))
        print("%-18s sharpness %6.2f  fine noise %7.2f  detail 1/2/4/8px %s  brightness %4.1f" % (
            name, sharp, noise, detail, part.mean() * 100))
        tile = Image.fromarray((part * 255).astype(np.uint8)).resize((cw * scale, ch * scale), Image.NEAREST)
        ImageDraw.Draw(tile).text((8, 8), name, fill=(255, 255, 255))
        tiles.append(tile)
    cols = 2 if len(tiles) > 2 else len(tiles)
    rows = (len(tiles) + cols - 1) // cols
    tw, th = tiles[0].size
    sheet = Image.new("RGB", (tw * cols, th * rows))
    for i, tile in enumerate(tiles):
        sheet.paste(tile, ((i % cols) * tw, (i // cols) * th))
    sheet.save(os.path.join(out, "compare.png"))
    print("wrote", os.path.join(out, "compare.png"))


if __name__ == "__main__":
    main()
