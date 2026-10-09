"""List every draw of a RenderDoc capture with its vertex and pixel shader debug names, vertex count and targets.

Run inside RenderDoc's embedded Python (qrenderdoc.exe --python tools/rdc_drawnames.py) with:
  ER_RDC   path to the .rdc capture
  ER_OUT   output text file
  ER_MATCH optional: only list draws whose vertex shader name contains one of these comma-separated strings
One line per draw: event, vertices (indices x instances), VS name, PS name, render target size and count, and whether
depth is written.
"""
import os
import re
import traceback

import renderdoc as rd

NAME_RE = re.compile(rb"[\\/]([A-Za-z0-9_\[\]@\.\-]+)\.[pcv]po\.pdb")
MATCH = [m for m in os.environ.get("ER_MATCH", "").split(",") if m]


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
        textures = {int(t.resourceId): t for t in controller.GetTextures()}
        draws = []

        def walk(actions):
            for a in actions:
                if a.flags & rd.ActionFlags.Drawcall:
                    draws.append(a)
                walk(a.children)

        walk(controller.GetRootActions())
        for a in draws:
            controller.SetFrameEvent(a.eventId, False)
            pipe = controller.GetPipelineState()
            vs = debug_name(pipe.GetShaderReflection(rd.ShaderStage.Vertex))
            if MATCH and not any(m in vs for m in MATCH):
                continue
            ps = debug_name(pipe.GetShaderReflection(rd.ShaderStage.Pixel))
            targets = [textures.get(int(d.resource)) for d in pipe.GetOutputTargets()]
            targets = [t for t in targets if t is not None]
            size = "%dx%d" % (targets[0].width, targets[0].height) if targets else "-"
            depth = controller.GetD3D12PipelineState().outputMerger.depthStencilState
            lines.append("%d %d %s %s rt=%d %s zwrite=%d" % (
                a.eventId, a.numIndices * max(1, a.numInstances), vs, ps, len(targets), size, depth.depthWrites))
    except Exception:
        lines.append(traceback.format_exc())
    finally:
        controller.Shutdown()
        cap.Shutdown()
        with open(os.environ["ER_OUT"], "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
        os._exit(0)


main()
