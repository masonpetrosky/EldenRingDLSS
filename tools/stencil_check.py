"""Show which stencil classes the game's TAA sees per pixel, from the add-on's TAA dump (remote command "mv N name").

usage: stencil_check.py <shots dir> <out dir> <name> [--region label x0 y0 x1 y1 ...]
Reads shots/<name>_stencil_00.bin (g_StencilTex, R8_UINT) and <name>_color_00.bin. TempAA uses the low two bits
(s & 3) and the next two ((s >> 2) & 3) to pick history weights from CB_AA.g_FeedbackWeightScale. Prints the share
of each stencil value over the frame and per region, and writes one brightness mask per value present (white = has
that value) plus mask images per TAA field.
"""
import os
import sys

import numpy as np
from PIL import Image

import mv_check

DEFAULT_REGIONS = {  # 4K pixel boxes from the first Limgrave grass dump: x0, y0, x1, y1
    "grass lower left": (100, 1500, 1500, 2100),
    "grass right": (2300, 1300, 3800, 2100),
    "tree canopy": (2000, 50, 2600, 600),
    "rock center": (1560, 740, 1980, 860),
    "stone ruin": (2700, 620, 3100, 900),
    "character torso": (1720, 1100, 2060, 1500),
    "sky": (2700, 50, 3300, 200),
}


def main():
    args = sys.argv[1:]
    regions = DEFAULT_REGIONS
    if "--region" in args:
        i = args.index("--region")
        rest, args = args[i + 1:], args[:i]
        regions = {rest[j]: tuple(int(v) for v in rest[j + 1:j + 5]) for j in range(0, len(rest), 5)}
    shots, out, name = args
    os.makedirs(out, exist_ok=True)
    w, h = (int(v) for v in open(os.path.join(shots, name + "_stencil_00.txt")).read().split()[:2])
    st = np.fromfile(os.path.join(shots, name + "_stencil_00.bin"), dtype=np.uint8).reshape(h, w)
    lum = mv_check.luminance(mv_check.decode_r11g11b10(mv_check.frames(shots, name, "color")[0]))
    scene = mv_check.to_display(lum, 0.18 / max(float(np.median(lum)), 1e-6))

    values, counts = np.unique(st, return_counts=True)
    print("stencil values over the frame:")
    for v, c in zip(values, counts):
        print("  %3d (0x%02x, low %d, next %d): %5.1f%%" % (v, v, v & 3, (v >> 2) & 3, 100 * c / st.size))
    print("\nper region (values covering at least 1%):")
    for label, (x0, y0, x1, y1) in regions.items():
        r = st[y0:y1, x0:x1]
        vs, cs = np.unique(r, return_counts=True)
        share = sorted(zip(cs / r.size, vs), reverse=True)
        print("  %-16s %s" % (label, "  ".join("%d: %4.1f%%" % (v, 100 * s) for s, v in share if s >= 0.01)))

    for v in values:
        mask = np.where(st == v, 1.0, scene * 0.35)
        mv_check.save(mask, os.path.join(out, "%s_stencil_%d.png" % (name, v)), 2)
    for field, shift in (("low", 0), ("next", 2)):
        f = (st >> shift) & 3
        # Brightness by field value: 0 = dim scene, 1/2/3 = increasingly bright.
        img = np.where(f == 0, scene * 0.3, 0.4 + 0.2 * f)
        mv_check.save(img, os.path.join(out, "%s_field_%s.png" % (name, field)), 2)
    print("wrote images to", out)


if __name__ == "__main__":
    main()
