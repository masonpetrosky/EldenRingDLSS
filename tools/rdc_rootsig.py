"""Print D3D12 root-signature parameters and what is bound to them at given events (ER_RDC, ER_OUT, ER_ROOT)."""
import os
import traceback
import renderdoc as rd

def attrs(obj):
    out = {}
    for a in dir(obj):
        if a.startswith("_"):
            continue
        try:
            v = getattr(obj, a)
        except Exception:
            continue
        if not callable(v):
            out[a] = v
    return out

try:
    cap = rd.OpenCaptureFile()
    cap.OpenFile(os.environ["ER_RDC"], "", None)
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    lines = []
    for eid in [int(x) for x in os.environ["ER_ROOT"].split(",")]:
        controller.SetFrameEvent(eid, True)
        d3d = controller.GetD3D12PipelineState()
        lines.append("=== event %d" % eid)
        lines.append("D3D12State fields: %s" % sorted(attrs(d3d).keys()))
        for key in ("rootSignature", "rootParams", "rootElements", "descriptorHeaps"):
            if hasattr(d3d, key):
                val = getattr(d3d, key)
                lines.append("%s = %s" % (key, val))
                try:
                    for i, el in enumerate(val):
                        lines.append("  [%d] %s" % (i, {k: str(v)[:160] for k, v in attrs(el).items()}))
                except TypeError:
                    lines.append("  fields: %s" % {k: str(v)[:300] for k, v in attrs(val).items()})
                    for i, param in enumerate(getattr(val, "parameters", [])):
                        fields = {k: v for k, v in attrs(param).items() if k not in ("this", "thisown")}
                        lines.append("    param[%d] %s" % (i, {k: str(v)[:120] for k, v in fields.items()}))
                        for j, rng in enumerate(fields.get("tableRanges", []) or []):
                            lines.append("       range[%d] %s" % (j, {k: str(v) for k, v in attrs(rng).items() if k not in ("this", "thisown")}))
        # descriptor-level view through the generic API: constant blocks with their root/descriptor source
        pipe = controller.GetPipelineState()
        for stage in (rd.ShaderStage.Vertex, rd.ShaderStage.Pixel):
            try:
                for used in pipe.GetConstantBlocks(stage, True):
                    lines.append("  %s CB access=%s desc.resource=%s off=%d size=%d" % (
                        stage, {k: str(v) for k, v in attrs(used.access).items()},
                        used.descriptor.resource, used.descriptor.byteOffset, used.descriptor.byteSize))
            except Exception as exc:
                lines.append("  GetConstantBlocks failed: %s" % exc)
    with open(os.environ["ER_OUT"], "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    controller.Shutdown(); cap.Shutdown()
except Exception:
    with open(os.environ["ER_OUT"], "w", encoding="utf-8") as f:
        f.write(traceback.format_exc())
finally:
    os._exit(0)
