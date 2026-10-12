"""Sum a GPU profile (dev remote "gpuprofile <frames> <name>", shots/gpuprofile_<name>.csv) per pass.

usage: gpu_profile.py <csv> [--top N] [--compare <csv2>]
Each CSV line is one segment of one command list: the GPU time from its timestamp to the next one in the same list.
Segments are grouped into passes: one-draw passes (post-processing) by shader and targets, many-draw passes by the
material's pass suffix (_Gbuf, _Dpt, _Velo, ...) and targets, compute by shader with variant hashes folded, plus ray
dispatches, acceleration-structure builds and the add-on's own passes. Prints milliseconds per frame per pass, and a
coarse summary by category. Segments on different queues can overlap, so the sum can exceed the frame time.
"""
import collections
import csv
import re
import sys

HEXISH = re.compile(r"(?<=_)[0-9a-f]{3,}[0-9a-z]*|\d+")


def fold(name):
    """Folds variant numbers and hashes out of a shader name (SpeedTree_5010f_w3129a7_Animation -> SpeedTree_#_w#_Animation)."""
    return HEXISH.sub("#", name)


def pass_key(label, draws):
    if label.startswith(("ERDLSS", "DispatchRays", "AccelerationStructureBuild")):
        return label
    if label.startswith("CS "):
        return "CS " + fold(label[3:])
    shader, _, targets = label.partition(" | ")
    if draws > 3 and "]" in shader:
        suffix = shader[shader.rfind("]") + 1:].split("@")[0]
        return "draws *%s | %s" % (suffix, targets)
    if draws > 3:
        return "draws %s | %s" % (fold(shader.split("@")[0]), targets)
    return "%s | %s" % (shader.split("@")[0], targets)


def category(key):
    if key.startswith("ERDLSS"):
        return "add-on: " + key[7:]
    if key.startswith("AccelerationStructureBuild"):
        return "ray tracing: scene build"
    if key.startswith("DispatchRays"):
        return "ray tracing: rays"
    if key.startswith("CS") and "Animation" in key:
        return "ray tracing: animation (compute)"
    if key.startswith("CS"):
        return "compute (other)"
    targets = key.partition(" | ")[2]
    if "_Velo" in key or "FurDir" in key:
        return "velocity"
    if targets.startswith("5rt"):
        return "G-buffer"
    if targets.startswith("0rt"):
        return "depth only (shadow maps, prepasses)"
    if key.startswith("draws"):
        return "other draws"
    return "full-screen passes"


def load(path):
    frames = set()
    per_key = collections.defaultdict(float)
    per_key_segments = collections.Counter()
    per_key_draws = collections.Counter()
    with open(path, newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            frames.add(row["frame"])
            draws = int(row["draws"])
            key = pass_key(row["label"], draws)
            per_key[key] += float(row["us"])
            per_key_segments[key] += 1
            per_key_draws[key] += draws
    n = max(1, len(frames))
    return n, {k: v / n / 1000.0 for k, v in per_key.items()}, per_key_segments, per_key_draws


def main():
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    top = 45
    compare = None
    if "--top" in args:
        top = int(args[args.index("--top") + 1])
    if "--compare" in args:
        compare = args[args.index("--compare") + 1]
    n, ms, segments, draws = load(args[0])
    print("%s: %d frames, %.2f ms of segments per frame" % (args[0], n, sum(ms.values())))
    cats = collections.defaultdict(float)
    for k, v in ms.items():
        cats[category(k)] += v
    print("\nby category (ms per frame):")
    for c, v in sorted(cats.items(), key=lambda kv: -kv[1]):
        print("  %7.2f  %s" % (v, c))
    other = load(compare)[1] if compare else None
    print("\ntop passes (ms per frame, segments and draws per frame):")
    for k, v in sorted(ms.items(), key=lambda kv: -kv[1])[:top]:
        extra = "  (%+.2f)" % (v - other.get(k, 0.0)) if other is not None else ""
        print("  %7.3f  %5.1f seg %7.1f draws  %s%s" % (v, segments[k] / n, draws[k] / n, k[:120], extra))


if __name__ == "__main__":
    main()
