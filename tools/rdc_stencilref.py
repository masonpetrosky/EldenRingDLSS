"""The stencil state of every skinned G-buffer and velocity draw: whether it writes the stencil, how, and the
reference value (the object class the add-on's NPC motion tells the player by, StencilClass in src/addon.cpp).

Run inside RenderDoc's embedded Python (qrenderdoc.exe --python tools/rdc_stencilref.py) with:
  ER_RDC   path to the .rdc capture
  ER_OUT   output text file
Writes one line per draw, then the draw count per material and stencil state.
"""
import os
import re
import traceback
from collections import Counter

import renderdoc as rd

NAME_RE = re.compile(rb"[\\/]([A-Za-z0-9_\[\]@\.\-]+)\.[pcv]po\.pdb")


def debug_name(refl):
    if refl is None:
        return "-"
    m = NAME_RE.search(bytes(refl.rawBytes))
    return m.group(1).decode(errors="replace") if m else "?"


def main():
    lines = []
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(os.environ["ER_RDC"], "", None) != rd.ResultCode.Succeeded:
        raise RuntimeError("cannot open capture")
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if result != rd.ResultCode.Succeeded:
        raise RuntimeError("cannot replay capture: %s" % result)
    try:
        draws = []

        def walk(actions):
            for a in actions:
                if a.flags & rd.ActionFlags.Drawcall:
                    draws.append(a)
                walk(a.children)

        walk(controller.GetRootActions())
        summary = Counter()
        for a in draws:
            controller.SetFrameEvent(a.eventId, False)
            pipe = controller.GetPipelineState()
            vs = debug_name(pipe.GetShaderReflection(rd.ShaderStage.Vertex))
            if "_Skin" not in vs or ("_Gbuf" not in vs and "_Velo" not in vs):
                continue
            d3d = controller.GetD3D12PipelineState()
            ds = d3d.outputMerger.depthStencilState
            ff = ds.frontFace
            key = (vs.split("@")[0], ds.stencilEnable, ff.passOperation, ff.function, ff.reference, ff.writeMask,
                   ff.compareMask)
            summary[key] += 1
            lines.append("%d %s stencil %d pass %s func %s ref %d write %02x read %02x depth %d/%d" % (
                a.eventId, vs, ds.stencilEnable, ff.passOperation, ff.function, ff.reference, ff.writeMask,
                ff.compareMask, ds.depthEnable, ds.depthWrites))
        lines.append("")
        lines.append("summary (material, stencil on, pass op, func, ref, write mask, read mask): draws")
        for key, n in sorted(summary.items(), key=lambda kv: str(kv[0])):
            lines.append("%s: %d" % (key, n))
    finally:
        controller.Shutdown()
        cap.Shutdown()
    with open(os.environ["ER_OUT"], "w") as f:
        f.write("\n".join(lines) + "\n")


try:
    main()
except Exception:
    with open(os.environ["ER_OUT"], "w") as f:
        f.write(traceback.format_exc())
os._exit(0)
