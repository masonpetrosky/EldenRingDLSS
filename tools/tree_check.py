"""Check whether last frame's SpeedTree wind can be rebuilt from this frame's (dev remote "treedump", ab_capture.ps1
-Round treedump).

usage: tree_check.py <shots dir>
Reads shots/treedump.txt (frame, shader, trees, offset per draw) and treedump.bin (18 float4s per tree: position and
index, then the 17-float4 wind entry). Trees are
matched between consecutive frames by shader and position. A tree model is a shader plus the wind anchor; the other
wind parameters (sway distance, strength, ...) drift every frame with gusts. Prints:
- whether all trees of a model share their clock differences within a frame (one per-tree offset on every clock),
- the per-frame clock advance per model and how much it differs between trees of the model,
- the predictor test: last frame's clocks of every matched tree rebuilt from one tree per model, and the error,
- how many trees belong to a model with at least one tree matched with the previous frame.
"""
import collections
import os
import sys

import numpy as np

# The wind clocks in a 17-float4 sSpeedTreeWind: (float4, component).
CLOCKS = {"glob": (2, 0), "br1": (4, 0), "br2": (6, 0), "l1rip": (8, 0), "l1tum": (8, 3), "l1tw": (10, 1),
          "l2rip": (11, 0), "l2tum": (11, 3), "l2tw": (13, 1), "frond": (14, 0)}
CLOCK_FLAT = [r * 4 + c for r, c in CLOCKS.values()]


def load(shots):
    data = np.fromfile(os.path.join(shots, "treedump.bin"), dtype=np.float32).reshape(-1, 18, 4)
    trees = []  # (frame, shader, model key, position, clocks)
    for line in open(os.path.join(shots, "treedump.txt"), encoding="utf-8"):
        frame, shader, count, offset = line.split()
        frame, count, offset = int(frame), int(count), int(offset) // 18
        for t in data[offset:offset + count]:
            wind = t[1:].reshape(-1)
            key = (shader.split("@")[0], tuple(np.round(wind[4:7], 4)))  # shader and wind anchor
            trees.append((frame, shader, key, tuple(np.round(t[0, :3], 3)), wind[CLOCK_FLAT].astype(np.float64)))
    return trees


def main():
    trees = load(sys.argv[1])
    frames = sorted({t[0] for t in trees})
    models = {t[2] for t in trees}
    print("%d frames (%d to %d), %d tree records, %d models, %d draws per frame on average" % (
        len(frames), frames[0], frames[-1], len(trees), len(models),
        sum(1 for _ in open(os.path.join(sys.argv[1], "treedump.txt"))) / max(1, len(frames))))

    # 1. Within a frame, clock_c - clock_global should be the same for every tree of a model.
    spread = []
    by_frame_model = collections.defaultdict(list)
    for frame, _, key, pos, clocks in trees:
        by_frame_model[(frame, key)].append((pos, clocks))
    for group in by_frame_model.values():
        clocks = np.array([c for _, c in group])
        used = np.all(clocks != 0, axis=0)  # unused clocks stay 0 and carry no offset
        if len(clocks) > 1 and used.any():
            spread.append(np.max(np.ptp((clocks - clocks[:, :1])[:, used], axis=0)))
    if spread:
        print("\n1. per-tree offset shared by all clocks: max spread of (clock - global clock) within a model %.2e "
              "(over %d model-frames with 2+ trees)" % (max(spread), len(spread)))

    # 2. Per-frame advance, from trees matched by model and position.
    index = {(t[0], t[2], t[3]): t[4] for t in trees}
    advances = collections.defaultdict(list)  # (frame, key) -> list of per-tree advance vectors
    for (frame, key, pos), clocks in index.items():
        prev = index.get((frame - 1, key, pos))
        if prev is not None:
            advances[(frame, key)].append(clocks - prev)
    if not advances:
        print("\nno tree matched between consecutive frames")
        return
    worst = max(np.max(np.ptp(np.array(v), axis=0)) for v in advances.values() if len(v) > 1) \
        if any(len(v) > 1 for v in advances.values()) else 0.0
    print("\n2. clock advance per frame: max difference between trees of one model %.2e" % worst)
    names = list(CLOCKS)
    for i, ((frame, key), v) in enumerate(sorted(advances.items(), key=lambda kv: (kv[0][1][0], kv[0][0]))):
        if i >= 12:
            break
        a = np.median(np.array(v), axis=0)
        print("  frame %d %-34s %2d trees: %s" % (frame, key[0][:34], len(v),
                                                 " ".join("%s %.4f" % (n, x)
                                                          for n, x in zip(names, a) if x != 0)))

    # 3. Predictor: last frame's clocks of every matched tree from the first matched tree of its model.
    errors, scale = [], []
    for (frame, key), _ in advances.items():
        matched = [(pos, c) for (f, k, pos), c in index.items() if f == frame and k == key and (f - 1, k, pos) in index]
        ref_pos, ref = matched[0]
        delta = ref - index[(frame - 1, key, ref_pos)]
        for pos, c in matched[1:]:
            errors.append(np.max(np.abs((c - delta) - index[(frame - 1, key, pos)])))
            scale.append(np.max(np.abs(delta)))
    if errors:
        print("\n3. predictor (one tree per model): %d trees, max error %.2e, median %.2e (typical advance %.3f)" % (
            len(errors), max(errors), float(np.median(errors)), float(np.median(scale))))

    # 4. Coverage: trees of a model with a matched tree in the previous frame.
    covered = total = 0
    for frame, _, key, pos, _ in trees:
        if frame == frames[0]:
            continue
        total += 1
        covered += (frame, key) in advances
    print("\n4. trees whose model has a tree matched with the previous frame: %d of %d (%.1f%%)" % (
        covered, total, 100.0 * covered / max(1, total)))


main()
