"""List draws/dispatches in an event range with shader debug names, SRV names and cbuffer names.
Env: ER_RDC, ER_OUT, ER_FIRST, ER_LAST."""
import os
import re
import traceback
import renderdoc as rd

NAME_RE = re.compile(rb"[\/]([A-Za-z0-9_\[\]@\.\-]+)\.[pcv]po\.pdb")

def debug_name(refl):
    if refl is None:
        return "-"
    m = NAME_RE.search(bytes(refl.rawBytes))
    return m.group(1).decode(errors="replace") if m else "?"

try:
    cap = rd.OpenCaptureFile()
    cap.OpenFile(os.environ["ER_RDC"], "", None)
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    first, last = int(os.environ["ER_FIRST"]), int(os.environ["ER_LAST"])
    textures = {int(t.resourceId): t for t in controller.GetTextures()}
    flat = []
    def walk(actions):
        for a in actions:
            if a.flags & (rd.ActionFlags.Drawcall | rd.ActionFlags.Dispatch) and first <= a.eventId <= last:
                flat.append(a)
            walk(a.children)
    walk(controller.GetRootActions())
    lines = []
    for a in flat:
        controller.SetFrameEvent(a.eventId, False)
        pipe = controller.GetPipelineState()
        is_cs = bool(a.flags & rd.ActionFlags.Dispatch)
        stage = rd.ShaderStage.Compute if is_cs else rd.ShaderStage.Pixel
        refl = pipe.GetShaderReflection(stage)
        srvs = []
        for used in pipe.GetReadOnlyResources(stage, True):
            idx = used.access.index
            label = refl.readOnlyResources[idx].name if refl and 0 <= idx < len(refl.readOnlyResources) else "#%d" % idx
            t = textures.get(int(used.descriptor.resource))
            srvs.append("%s=%s" % (label, ("%dx%d %s" % (t.width, t.height, t.format.Name())) if t else "buf"))
        cbs = [c.name for c in refl.constantBlocks] if refl else []
        outs = []
        if not is_cs:
            for d in pipe.GetOutputTargets():
                t = textures.get(int(d.resource))
                if t:
                    outs.append("%dx%d %s" % (t.width, t.height, t.format.Name()))
        else:
            for used in pipe.GetReadWriteResources(stage, True):
                t = textures.get(int(used.descriptor.resource))
                outs.append("uav " + (("%dx%d %s" % (t.width, t.height, t.format.Name())) if t else "buf"))
        lines.append("%d %s %s | srv: %s | cb: %s | out: %s" % (a.eventId, "CS" if is_cs else "PS", debug_name(refl),
                                                          "; ".join(srvs), ",".join(cbs), "; ".join(outs)))
    with open(os.environ["ER_OUT"], "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    controller.Shutdown(); cap.Shutdown()
except Exception:
    with open(os.environ["ER_OUT"], "w", encoding="utf-8") as f:
        f.write(traceback.format_exc())
finally:
    os._exit(0)
