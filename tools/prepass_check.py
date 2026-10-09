"""Compare the prepass replacement with the velocity-draw path (ab_capture.ps1 -Round windprepass).

usage: prepass_check.py <shots dir> <out dir>
Coverage: in the pre_<mode>_drawn / _merged dumps the merge marks pixels the grass pass drew / the merge kept with the
raw value (1, 1); counted over vegetation pixels (stencil class 3) per distance band (stencil bits 2-3). Motion: the
share and median of moving vegetation pixels in pre_off / pre_on. Frame times from the PresentMon logs pre_wind_off,
pre_draws, pre_replaced and pre_wind_off2. Writes brightness-coded coverage images (white = merged, grey = drawn but
not merged, black = not drawn).
"""
import os
import sys

import numpy as np

import mv_check
import wind_check


def marked(shots, name):
    mv = mv_check.frames(shots, name, "mv")[0]
    w, h = (int(v) for v in open(os.path.join(shots, name + "_stencil_00.txt")).read().split()[:2])
    stencil = np.fromfile(os.path.join(shots, name + "_stencil_00.bin"), dtype=np.uint8).reshape(h, w)
    return (mv[..., 0] == 1) & (mv[..., 1] == 1), stencil


def main():
    shots, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    for mode in ("off", "on"):
        drawn, stencil = marked(shots, "pre_%s_drawn" % mode)
        merged, _ = marked(shots, "pre_%s_merged" % mode)
        veg = (stencil & 3) == 3
        print("prepass replacement %s:" % mode)
        for band in range(4):
            sel = veg & (((stencil >> 2) & 3) == band)
            if sel.sum() < 1000:
                continue
            print("  band %d: %5.1f%% of screen, drawn %5.1f%%, merged %5.1f%%" % (
                band, 100 * sel.mean(), 100 * drawn[sel].mean(), 100 * merged[sel].mean()))
        print("  all vegetation: drawn %5.1f%%, merged %5.1f%%" % (100 * drawn[veg].mean(), 100 * merged[veg].mean()))
        img = np.where(merged, 1.0, np.where(drawn, 0.4, 0.0)).astype(np.float32)
        mv_check.save(img, os.path.join(out, "coverage_%s.png" % mode), 2)

        mvs = [mv_check.motion_px(m) for m in mv_check.frames(shots, "pre_%s" % mode, "mv")]
        peak = np.max([np.hypot(m[..., 0], m[..., 1]) for m in mvs], axis=0)
        w, h = (int(v) for v in open(os.path.join(shots, "pre_%s_stencil_00.txt" % mode)).read().split()[:2])
        st = np.fromfile(os.path.join(shots, "pre_%s_stencil_00.bin" % mode), dtype=np.uint8).reshape(h, w)
        v = (st & 3) == 3
        moving = peak[v] > 0.05
        print("  motion: %5.1f%% of vegetation moving, median %.2f px, p90 %.2f px" % (
            100 * moving.mean(), np.median(peak[v][moving]) if moving.any() else 0,
            np.percentile(peak[v][moving], 90) if moving.any() else 0))
        mv_check.save(np.sqrt(np.clip(peak / 2.0, 0, 1)), os.path.join(out, "motion_%s.png" % mode), 2)

    for name in ("pre_wind_off", "pre_draws", "pre_replaced", "pre_wind_off2"):
        t = wind_check.frame_times(os.path.join(shots, name + ".csv"))
        if t is None or len(t[0]) == 0:
            print("%-14s no log" % name)
            continue
        print("%-14s %.2f ms per frame, GPU busy %.2f ms, p99 %.2f ms (%d frames)" % (
            name, t[0].mean(), t[1].mean() if len(t[1]) else float("nan"), np.percentile(t[0], 99), len(t[0])))


main()
