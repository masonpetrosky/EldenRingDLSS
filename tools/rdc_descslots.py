"""The descriptor heap slots some events read, and which other events of the frame read the same slots: whether the
game gives each draw its own copies of its descriptor tables (a ring) or shares them, which decides whether the add-on
may overwrite a slot for one draw (the water reflections' RedirectShaderResource).

Run inside RenderDoc's embedded Python (qrenderdoc.exe --python tools/rdc_descslots.py) with:
  ER_RDC    path to the .rdc capture
  ER_OUT    output text file
  ER_EIDS   comma-separated event IDs to report
"""
import os
import traceback
from collections import defaultdict

import renderdoc as rd


def main():
    out = []
    cap = rd.OpenCaptureFile()
    cap.OpenFile(os.environ["ER_RDC"], "", None)
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    targets = [int(x) for x in os.environ["ER_EIDS"].split(",") if x.strip()]

    actions = []

    def walk(items):
        for a in items:
            if a.flags & (rd.ActionFlags.Drawcall | rd.ActionFlags.Dispatch):
                actions.append(a.eventId)
            walk(a.children)

    walk(controller.GetRootActions())
    slot_users = defaultdict(list)  # (store, offset) -> events
    target_info = {}
    for eid in actions:
        controller.SetFrameEvent(eid, False)
        try:
            accesses = controller.GetDescriptorAccess()
        except Exception as exc:
            out.append("%d: %s" % (eid, exc))
            continue
        pipe = controller.GetPipelineState() if eid in targets else None
        rows = []
        for acc in accesses:
            if acc.staticallyUnused:
                continue
            key = (int(acc.descriptorStore), int(acc.byteOffset))
            slot_users[key].append(eid)
            if pipe is not None:
                cat = rd.CategoryForDescriptorType(acc.type)
                reg = "?"
                try:
                    refl = pipe.GetShaderReflection(acc.stage)
                    if cat == rd.DescriptorCategory.ReadOnlyResource:
                        r = refl.readOnlyResources[acc.index]
                        reg = "t%d %s" % (r.fixedBindNumber, r.name)
                    elif cat == rd.DescriptorCategory.ConstantBlock:
                        r = refl.constantBlocks[acc.index]
                        reg = "b%d %s" % (r.fixedBindNumber, r.name)
                    elif cat == rd.DescriptorCategory.Sampler:
                        r = refl.samplers[acc.index]
                        reg = "s%d %s" % (r.fixedBindNumber, r.name)
                    elif cat == rd.DescriptorCategory.ReadWriteResource:
                        r = refl.readWriteResources[acc.index]
                        reg = "u%d %s" % (r.fixedBindNumber, r.name)
                except Exception as exc:
                    reg = "? (%s)" % exc
                res = "?"
                try:
                    d = controller.GetDescriptors(acc.descriptorStore, [rd.DescriptorRange(acc)])
                    res = int(d[0].resource)
                except Exception as exc:
                    res = "? (%s)" % exc
                rows.append((str(acc.stage).replace("ShaderStage.", ""), reg, key, res))
        if pipe is not None:
            target_info[eid] = rows
    for eid in targets:
        out.append("== %d" % eid)
        for stage, reg, key, res in target_info.get(eid, []):
            users = slot_users[key]
            others = [e for e in users if e != eid]
            out.append("  %-6s %-40s store %d off %8d res %s  shared with %d events %s" % (
                stage, reg, key[0], key[1], res, len(others), others[:12]))
    with open(os.environ["ER_OUT"], "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")


try:
    main()
except Exception:
    with open(os.environ["ER_OUT"], "w", encoding="utf-8") as f:
        f.write(traceback.format_exc())
finally:
    os._exit(0)
