"""Check which parts of the scene carry motion vectors, from the add-on's TAA dump (remote command "mv N name").

usage: mv_check.py <shots dir> <out dir> <name> [--crop x y w h] [--compare <name2>]
Reads shots/<name>_mv_NN.bin (g_MoveTex, R16G16_FLOAT) and <name>_color_NN.bin (SourceTex, R11G11B10_FLOAT).
With the camera at rest, the camera part of the motion is zero, so any nonzero motion comes from objects that
write their own velocity. Prints per-frame statistics (whole frame and the crop) and writes brightness-coded
images: the scene, motion magnitude (black = none, white = 2 px or more) and an overlay with moving pixels in white.
--compare takes a second dump of the same view taken a moment later: pixels whose color changed between the two
but that have no motion in either dump are moving without motion vectors.
"""
import glob
import os
import sys

import numpy as np
from PIL import Image

THRESHOLDS = (0.01, 0.05, 0.25, 1.0)  # px


def frames(shots, name, kind):
    paths = sorted(glob.glob(os.path.join(shots, "%s_%s_[0-9][0-9].bin" % (name, kind))))
    if not paths:
        sys.exit("no %s_%s_NN.bin in %s" % (name, kind, shots))
    out = []
    for p in paths:
        w, h = (int(v) for v in open(p[:-4] + ".txt").read().split()[:2])
        out.append(np.fromfile(p, dtype=np.float16 if kind == "mv" else np.uint32))
        out[-1] = out[-1].reshape(h, w, 2).astype(np.float32) if kind == "mv" else out[-1].reshape(h, w)
    return out


def motion_px(mv):
    """Motion (current - previous) in pixels. Both stored axes are in screen heights (see docs/recon.md)."""
    return mv * mv.shape[0]


def decode_r11g11b10(v):
    def small_float(bits, mant_bits):
        exp = (bits >> mant_bits) & 0x1F
        mant = (bits & ((1 << mant_bits) - 1)).astype(np.float32) / (1 << mant_bits)
        return np.where(exp == 0, mant * 2.0 ** -14, (1 + mant) * np.exp2(exp.astype(np.float32) - 15))
    return np.stack([small_float(v & 0x7FF, 6), small_float((v >> 11) & 0x7FF, 6), small_float(v >> 22, 5)], -1)


def luminance(rgb):
    return rgb @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)


def to_display(lum, scale):
    x = lum * scale
    return np.clip(x / (1 + x), 0, 1) ** (1 / 2.2)


def stats_line(label, mag):
    parts = ["%5.1f%% >%gpx" % (100 * np.mean(mag > t), t) for t in THRESHOLDS]
    moving = mag[mag > 0.05]
    tail = "  median/p99 of moving %.2f/%.2f px" % (np.median(moving), np.percentile(moving, 99)) if moving.size else ""
    return "%-14s %s%s" % (label, "  ".join(parts), tail)


def save(img, path, downscale):
    h, w = img.shape[:2]
    Image.fromarray((np.clip(img, 0, 1) * 255).astype(np.uint8)).resize(
        (w // downscale, h // downscale), Image.LANCZOS if downscale > 1 else Image.NEAREST).save(path)


def main():
    args = sys.argv[1:]
    crop, compare = None, None
    if "--crop" in args:
        i = args.index("--crop")
        crop = tuple(int(v) for v in args[i + 1:i + 5])
        args = args[:i] + args[i + 5:]
    if "--compare" in args:
        i = args.index("--compare")
        compare = args[i + 1]
        args = args[:i] + args[i + 2:]
    shots, out, name = args
    os.makedirs(out, exist_ok=True)

    mvs = [motion_px(m) for m in frames(shots, name, "mv")]
    lums = [luminance(decode_r11g11b10(c)) for c in frames(shots, name, "color")]
    h, w = lums[0].shape
    scale = 0.18 / max(float(np.median(lums[0])), 1e-6)
    x, y, cw, ch = crop or (0, 0, w, h)

    print("%d frames of %dx%d; share of pixels moving more than each threshold" % (len(mvs), w, h))
    for i, mv in enumerate(mvs):
        mag = np.hypot(mv[..., 0], mv[..., 1])
        line = stats_line("frame %02d" % i, mag)
        if crop:
            c = mv[y:y + ch, x:x + cw]
            line += "\n" + stats_line("  crop", np.hypot(c[..., 0], c[..., 1]))
            line += "  mean crop motion %+.3f %+.3f px" % (c[..., 0].mean(), c[..., 1].mean())
        print(line)

    # Largest motion over all frames per pixel, so swaying that passes through zero still shows.
    peak = np.max([np.hypot(m[..., 0], m[..., 1]) for m in mvs], axis=0)
    print(stats_line("peak over all", peak))
    if crop:
        print(stats_line("  crop", peak[y:y + ch, x:x + cw]))

    scene = to_display(lums[0], scale)
    heat = np.sqrt(np.clip(peak / 2.0, 0, 1))
    overlay = np.where(peak > 0.05, 1.0, scene * 0.45)
    for label, img in (("scene", scene), ("motion", heat), ("overlay", overlay)):
        save(img, os.path.join(out, "%s_%s.png" % (name, label)), 2)
        if crop:
            save(np.repeat(np.repeat(img[y:y + ch, x:x + cw], 2, 0), 2, 1),
                 os.path.join(out, "%s_%s_crop.png" % (name, label)), 1)

    if compare:
        mvs2 = [motion_px(m) for m in frames(shots, compare, "mv")]
        lums2 = [luminance(decode_r11g11b10(c)) for c in frames(shots, compare, "color")]
        # Averaging each dump's frames evens out most of the sub-pixel jitter.
        a, b = to_display(np.mean(lums, 0), scale), to_display(np.mean(lums2, 0), scale)
        change = np.abs(a - b)
        peak_all = np.maximum(peak, np.max([np.hypot(m[..., 0], m[..., 1]) for m in mvs2], axis=0))
        changed = change > 0.06
        still = peak_all <= 0.05
        print("\n%s vs %s: %.1f%% of pixels changed brightness by more than 6%%" % (name, compare, 100 * changed.mean()))
        print("  of those, %.1f%% have motion vectors and %.1f%% have none" % (
            100 * np.mean(~still[changed]) if changed.any() else 0, 100 * np.mean(still[changed]) if changed.any() else 0))
        if crop:
            cc, cs = changed[y:y + ch, x:x + cw], still[y:y + ch, x:x + cw]
            print("  crop: %.1f%% changed; of those %.1f%% without motion vectors" % (
                100 * cc.mean(), 100 * np.mean(cs[cc]) if cc.any() else 0))
        # White: changed without motion vectors; gray: changed with motion vectors; dim scene elsewhere.
        missing = np.where(changed & still, 1.0, np.where(changed, 0.6, a * 0.3))
        save(missing, os.path.join(out, "%s_vs_%s_missing.png" % (name, compare)), 2)
        if crop:
            save(np.repeat(np.repeat(missing[y:y + ch, x:x + cw], 2, 0), 2, 1),
                 os.path.join(out, "%s_vs_%s_missing_crop.png" % (name, compare)), 1)
    print("wrote images to", out)


if __name__ == "__main__":
    main()
