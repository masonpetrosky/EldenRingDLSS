"""Compare the TAA motion vectors and frame times with wind motion vectors off and on (ab_capture.ps1 -Round wind).

usage: wind_check.py <shots dir> <out dir> [<off name> <on name>]   (defaults: windoff windon)
Per stencil class (low two bits: 0 terrain/architecture, 1 characters, 3 vegetation and natural rocks) prints the share
of pixels whose motion exceeds 0.05 px in any dumped frame and the median of those motions, for both dumps. Frame
times come from the PresentMon logs wind_off.csv / wind_on.csv. Writes brightness-coded images: motion with wind
motion vectors on (black = none, white = 2 px or more) and the scene.
"""
import csv
import os
import sys

import numpy as np

import mv_check

CLASSES = {0: "terrain/architecture", 1: "characters", 3: "vegetation/rocks"}


def load(shots, name):
    mvs = [mv_check.motion_px(m) for m in mv_check.frames(shots, name, "mv")]
    peak = np.max([np.hypot(m[..., 0], m[..., 1]) for m in mvs], axis=0)
    w, h = (int(v) for v in open(os.path.join(shots, name + "_stencil_00.txt")).read().split()[:2])
    stencil = np.fromfile(os.path.join(shots, name + "_stencil_00.bin"), dtype=np.uint8).reshape(h, w)
    return mvs, peak, stencil


def frame_times(path):
    if not os.path.exists(path):
        return None
    with open(path, encoding="utf-8-sig") as f:
        rows = [r for r in csv.DictReader(f) if r.get("Application", "").lower() == "eldenring.exe"]
    def column(key):
        return np.array([float(r[key]) for r in rows if r.get(key) not in (None, "", "NA")])
    return column("MsBetweenPresents"), column("MsGPUBusy")


def main():
    args = sys.argv[1:]
    shots, out = args[0], args[1]
    off_name, on_name = (args[2], args[3]) if len(args) >= 4 else ("windoff", "windon")
    os.makedirs(out, exist_ok=True)
    results = {}
    for label, name in (("off", off_name), ("on", on_name)):
        mvs, peak, stencil = load(shots, name)
        results[label] = (mvs, peak, stencil)
        print("wind motion vectors %s (%s, %d frames):" % (label, name, len(mvs)))
        low = stencil & 3
        for c, desc in CLASSES.items():
            sel = low == c
            if not sel.any():
                continue
            moving = peak[sel] > 0.05
            med = np.median(peak[sel][moving]) if moving.any() else 0.0
            p99 = np.percentile(peak[sel][moving], 99) if moving.any() else 0.0
            print("  class %d %-22s %5.1f%% of screen, %5.1f%% moving, median %.2f px, p99 %.2f px" % (
                c, desc, 100 * sel.mean(), 100 * moving.mean(), med, p99))

    # Does the motion change over consecutive frames (swaying) on vegetation?
    mvs, peak, stencil = results["on"]
    veg = ((stencil & 3) == 3) & (peak > 0.05)
    if veg.any() and len(mvs) > 1:
        series = np.array([np.hypot(m[..., 0], m[..., 1])[veg].mean() for m in mvs])
        print("  vegetation mean motion per frame (on):", " ".join("%.3f" % v for v in series), "px")

    for label, name in (("off", "wind_off.csv"), ("on", "wind_on.csv")):
        t = frame_times(os.path.join(shots, name))
        if t is None or len(t[0]) == 0:
            print("frame times %s: no log" % label)
            continue
        print("frame times %-3s: %.2f ms per frame (%.0f fps), GPU busy %.2f ms, p99 frame %.2f ms (%d frames)" % (
            label, t[0].mean(), 1000 / t[0].mean(), t[1].mean() if len(t[1]) else float("nan"),
            np.percentile(t[0], 99), len(t[0])))

    lum = mv_check.luminance(mv_check.decode_r11g11b10(mv_check.frames(shots, on_name, "color")[0]))
    scene = mv_check.to_display(lum, 0.18 / max(float(np.median(lum)), 1e-6))
    mv_check.save(scene, os.path.join(out, "wind_scene.png"), 2)
    for label in ("off", "on"):
        mv_check.save(np.sqrt(np.clip(results[label][1] / 2.0, 0, 1)), os.path.join(out, "wind_motion_%s.png" % label), 2)
    print("wrote images to", out)


if __name__ == "__main__":
    main()
