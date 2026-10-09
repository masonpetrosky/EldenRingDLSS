"""Summarize tools/rdc_pixelsource.py output: share of sampled pixels per material family, overall and per region.

usage: pixelsource_summary.py <pixelsource.txt> [name x0 y0 x1 y1 ...]
Material families: SpeedTree (all variants), the grass family covered by the wind port (Grass2, Vesitation,
Grass2_LOD1), and every other shader name as it is (without the @[cl] suffix). Pixels without a G-buffer draw (sky)
are left out.
"""
import collections
import sys

COVERED = ("M[AMSN_V]_Grass2_Gbuf", "M[AMSN_V]_Vesitation_Gbuf", "M[AMSN_V]_Grass2_LOD1_Gbuf")


def family(name):
    name = name.split("@")[0]
    if name.startswith("SpeedTree_"):
        return "SpeedTree (trees, shrubs)"
    if name in COVERED:
        return "grass family (covered)"
    return name


def main():
    samples = []
    for line in open(sys.argv[1], encoding="utf-8"):
        parts = line.split()
        if len(parts) == 4 and parts[2] != "0":
            samples.append((int(parts[0]), int(parts[1]), family(parts[3])))
    regions = [("whole frame", 0, 0, 3840, 2160)]
    args = sys.argv[2:]
    for i in range(0, len(args), 5):
        regions.append((args[i],) + tuple(int(v) for v in args[i + 1:i + 5]))
    for name, x0, y0, x1, y1 in regions:
        inside = [f for x, y, f in samples if x0 <= x < x1 and y0 <= y < y1]
        counts = collections.Counter(inside)
        print("%s (%d samples):" % (name, len(inside)))
        for f, n in counts.most_common(8):
            print("  %5.1f%%  %s" % (100.0 * n / len(inside), f))


main()
