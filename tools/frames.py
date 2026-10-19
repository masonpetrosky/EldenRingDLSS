"""Summarize PresentMon frame-time logs (tools/ab_capture.ps1 LogFrames: shots/<name>.csv).

usage: frames.py <csv> [<csv> ...]
Per log: frames, average fps (frames over their total time), 1% low (the average fps of the slowest 1% of frames)
and the median GPU busy time per frame. These are the README's frame-rate columns.
"""
import csv
import os
import sys

import numpy as np


def frame_times(path):
    with open(path, encoding="utf-8-sig") as f:
        rows = [r for r in csv.DictReader(f) if r.get("Application", "").lower() == "eldenring.exe"]

    def column(key):
        return np.array([float(r[key]) for r in rows if r.get(key) not in (None, "", "NA")])

    return column("MsBetweenPresents"), column("MsGPUBusy")


def main():
    print("%-22s %6s %8s %7s %9s" % ("log", "frames", "avg fps", "1% low", "GPU ms"))
    for path in sys.argv[1:]:
        between, gpu = frame_times(path)
        if len(between) == 0:
            print("%-22s no frames" % os.path.basename(path))
            continue
        slowest = np.sort(between)[::-1][:int(np.ceil(len(between) * 0.01))]
        print("%-22s %6d %8.1f %7.1f %9.1f" % (os.path.splitext(os.path.basename(path))[0], len(between),
                                               1000.0 / between.mean(), 1000.0 / slowest.mean(),
                                               np.median(gpu) if len(gpu) else float("nan")))


if __name__ == "__main__":
    main()
