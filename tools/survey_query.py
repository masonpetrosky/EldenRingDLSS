"""Query a survey directory produced by rdc_survey.py.

usage: survey_query.py <survey_dir> uses <tex_id> [...]   - where each texture is written and read
       survey_query.py <survey_dir> event <eid> [...]     - full detail line for events
       survey_query.py <survey_dir> range <first> <last>  - compact listing of an event range
"""
import re
import sys


def load(survey_dir):
    rows = []
    with open(survey_dir + "/actions.tsv", encoding="utf-8") as f:
        next(f)
        for line in f:
            eid, kind, name, markers, details = (line.rstrip("\n").split("\t") + [""] * 5)[:5]
            rows.append((int(eid), kind, name, details))
    return rows


def groups(details):
    """Split a details string into {'PS.srv': [...], 'RT': [...], ...}."""
    out = {}
    for key, body in re.findall(r"([A-Z]{2}\.(?:srv|uav)|RT|DS)\{(.*?)\}(?=\s{2}|$)", details):
        out[key] = [part.strip() for part in body.split(";")]
    return out


def short(details):
    shaders = " ".join(re.findall(r"(?:VS|PS|CS)=shader\d+", details))
    g = groups(details)
    texs = lambda key: ",".join(re.findall(r"tex(\d+)\[", " ".join(g.get(key, []))))
    parts = [shaders]
    for key in ("CS.srv", "PS.srv", "CS.uav", "PS.uav", "RT", "DS"):
        if key in g:
            parts.append("%s:%s" % (key, texs(key)))
    dims = re.search(r"dims=\([^)]*\)|indices=\d+ inst=\d+", details)
    return (dims.group(0) + " " if dims else "") + "  ".join(parts)


def main():
    survey_dir, cmd, args = sys.argv[1], sys.argv[2], sys.argv[3:]
    rows = load(survey_dir)
    if cmd == "uses":
        for tex in args:
            pattern = "tex%s[" % tex
            print("== tex%s" % tex)
            for eid, kind, name, details in rows:
                g = groups(details)
                writes = any(pattern in p for key in ("RT", "CS.uav", "PS.uav", "DS") for p in g.get(key, []))
                reads = any(pattern in p for key in ("CS.srv", "PS.srv", "VS.srv") for p in g.get(key, []))
                if writes or reads:
                    print("  %6d %-8s %s%s  %s" % (eid, kind, "W" if writes else " ", "R" if reads else " ",
                                                   short(details)))
    elif cmd == "event":
        wanted = {int(a) for a in args}
        for eid, kind, name, details in rows:
            if eid in wanted:
                print("== %d %s %s" % (eid, kind, name))
                for key, parts in groups(details).items():
                    print("  %s:" % key)
                    for part in parts:
                        print("     " + part)
                print("  " + " ".join(re.findall(r"(?:VS|PS|CS)=shader\d+|dims=\([^)]*\)|indices=\d+ inst=\d+",
                                                 details)))
    elif cmd == "range":
        first, last = int(args[0]), int(args[1])
        for eid, kind, name, details in rows:
            if first <= eid <= last and kind not in ("other",):
                print("%6d %-8s %-40.40s %s" % (eid, kind, name, short(details) if details else ""))


if __name__ == "__main__":
    main()
