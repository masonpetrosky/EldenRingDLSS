"""Temporal instability of a burst of add-on screenshots of a static camera (ab_capture.ps1 -Round water).

usage: flicker.py <shots dir> <out prefix> <name> [<name> ...] [--region label x y w h ...]
The shots are read as HDR10 (PQ), as the game outputs in HDR (see shots.py).
Writes <out prefix>_frame.png (first shot, half size) and <out prefix>_std.png (per-pixel temporal standard deviation
of the SDR preview's luma, x8, half size: bright = unstable). Prints the consecutive-shot difference (camera motion
check) and, per region, the mean temporal std and the 99th percentile.
"""
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shots  # noqa: E402

LUMA = np.array([0.2126, 0.7152, 0.0722], np.float32)


def main():
    args = sys.argv[1:]
    shots_dir, prefix = args[0], args[1]
    names, regions = [], []
    i = 2
    while i < len(args):
        if args[i] == "--region":
            regions.append((args[i + 1], *(int(v) for v in args[i + 2:i + 6])))
            i += 6
        else:
            names.append(args[i])
            i += 1
    frames = [shots.load(shots_dir, n, shots.HDR10_PQ) for n in names]
    lum = np.stack([f @ LUMA for f in frames])
    diffs = [float(np.mean(np.abs(lum[k + 1] - lum[k]))) * 1000 for k in range(len(lum) - 1)]
    print("consecutive mean |diff| x1000:", " ".join("%.2f" % d for d in diffs))
    std = lum.std(axis=0)
    print("whole frame: mean std x1000 %.2f, p99 %.1f" % (std.mean() * 1000, np.percentile(std, 99) * 1000))
    for label, x, y, w, h in regions:
        s = std[y:y + h, x:x + w]
        print("%-10s mean std x1000 %6.2f  p99 %6.1f  max %6.1f  brightness %4.1f" % (
            label, s.mean() * 1000, np.percentile(s, 99) * 1000, s.max() * 1000, lum[:, y:y + h, x:x + w].mean() * 100))
    if prefix != "-":
        Image.fromarray((frames[0] * 255 + 0.5).astype(np.uint8)).resize((1920, 1080), Image.LANCZOS).save(
            prefix + "_frame.png")
        Image.fromarray(np.clip(std * 8 * 255, 0, 255).astype(np.uint8)).resize((1920, 1080), Image.BOX).save(
            prefix + "_std.png")


if __name__ == "__main__":
    main()
