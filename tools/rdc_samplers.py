"""List samplers bound at given events with filter, anisotropy, LOD bias and range (ER_RDC, ER_OUT, ER_EIDS)."""
import os
import traceback
import renderdoc as rd

def attrs(obj):
    return {a: getattr(obj, a) for a in dir(obj) if not a.startswith("_") and not callable(getattr(obj, a, None))}

try:
    cap = rd.OpenCaptureFile()
    cap.OpenFile(os.environ["ER_RDC"], "", None)
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    lines = []
    for eid in [int(x) for x in os.environ["ER_EIDS"].split(",")]:
        controller.SetFrameEvent(eid, True)
        pipe = controller.GetPipelineState()
        lines.append("=== event %d" % eid)
        for stage in (rd.ShaderStage.Vertex, rd.ShaderStage.Pixel, rd.ShaderStage.Compute):
            try:
                samplers = pipe.GetSamplers(stage, True)
            except Exception as exc:
                lines.append("  %s GetSamplers failed: %s" % (stage, exc))
                continue
            for used in samplers:
                s = used.sampler
                a = attrs(s)
                keep = {k: str(v) for k, v in a.items() if k in ("filter", "maxAnisotropy", "mipBias", "minLOD", "maxLOD",
                                                                 "addressU", "compareFunction")}
                lines.append("  %s s%d %s" % (stage, used.access.index, keep))
    with open(os.environ["ER_OUT"], "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    controller.Shutdown(); cap.Shutdown()
except Exception:
    with open(os.environ["ER_OUT"], "w", encoding="utf-8") as f:
        f.write(traceback.format_exc())
finally:
    os._exit(0)
