"""Tree wind patches before their command lists against inline ones, from "treebatch alternate" motion dumps.

usage: tree_alternate.py <shots dir> <name>... [--scale 0.5]
In that mode the patches run before their lists in odd frames and inline in even ones, and each dumped frame's .txt
ends with the frame's patches in both placements (ab_capture.ps1 -Round treecheck). Neighboring frames see nearly the
same wind, so each frame is compared with the mean of its two neighbors, which used the other placement: per frame,
the mean motion of the vegetation in the upper part of the scene (mostly tree canopy) and the share of it that moves.
With the patches right in both placements the ratios stay near 1. Below full size the scene fills the top-left
render-size part of the dumped textures (--scale: the render scale).
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mv_check  # noqa: E402


def load(shots, name, scale):
    mvs = mv_check.frames(shots, name, "mv")
    h, w = mvs[0].shape[:2]
    sh, sw = int(round(h * scale)), int(round(w * scale))
    placements, stencils = [], []
    for i in range(len(mvs)):
        fields = open(os.path.join(shots, "%s_mv_%02d.txt" % (name, i))).read().split()
        before, inline = (int(v) for v in fields[5:7]) if len(fields) >= 7 else (0, 0)
        placements.append("before" if before > 0 and inline == 0 else "inline" if inline > 0 and before == 0 else "?")
        p = os.path.join(shots, "%s_stencil_%02d" % (name, i))
        sw_, sh_ = (int(v) for v in open(p + ".txt").read().split()[:2])
        stencils.append(np.fromfile(p + ".bin", dtype=np.uint8).reshape(sh_, sw_)[:sh, :sw] & 3)
    canopy = np.all([s == 3 for s in stencils], axis=0)
    canopy[int(sh * 0.52):] = False
    mags = [np.hypot(m[:sh, :sw, 0], m[:sh, :sw, 1])[canopy] * sh for m in mvs]
    return placements, mags


def main():
    args = sys.argv[1:]
    scale = 0.5
    if "--scale" in args:
        i = args.index("--scale")
        scale = float(args[i + 1])
        del args[i:i + 2]
    shots, names = args[0], args[1:]
    ratios = {"inline": [], "before": []}
    for name in names:
        placements, mags = load(shots, name, scale)
        means = [m.mean() for m in mags]
        moving = [np.mean(m > 0.02) for m in mags]
        print("== %s (%d canopy pixels)" % (name, mags[0].size))
        for k in range(len(mags)):
            line = "  frame %2d %-6s mean %.4f px, moving %.1f%%" % (k, placements[k], means[k], 100 * moving[k])
            if 0 < k < len(mags) - 1 and placements[k] in ratios and \
                    placements[k - 1] == placements[k + 1] != placements[k] and placements[k - 1] != "?":
                r = means[k] / ((means[k - 1] + means[k + 1]) / 2)
                ratios[placements[k]].append(r)
                line += ", / neighbors %.3f" % r
            print(line)
    for placement, values in ratios.items():
        if values:
            print("%s frames against their neighbors: geometric mean %.3f over %d frames (min %.3f, max %.3f)" % (
                placement, np.exp(np.mean(np.log(values))), len(values), min(values), max(values)))
    if ratios["inline"] and ratios["before"]:
        # Inline over before, from both directions.
        g = np.exp((np.mean(np.log(ratios["inline"])) - np.mean(np.log(ratios["before"]))) / 2)
        print("canopy motion inline / before their lists: %.3f" % g)


if __name__ == "__main__":
    main()
