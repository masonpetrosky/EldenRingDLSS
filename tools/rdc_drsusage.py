"""Which shaders of a capture reference the dynamic-resolution constants (cbSceneParam's
SC_DynamicResolution_ScreenPercentage and friends), by searching each bound shader's bytecode for the names.

Run inside RenderDoc's embedded Python (qrenderdoc.exe --python tools/rdc_drsusage.py) with:
  ER_RDC   path to the .rdc capture
  ER_OUT   output text file
One line per draw or dispatch: event, kind, render target size (or "-"), and per stage the shader debug name with
"+" if its bytecode names a DynamicResolution constant, "-" if it does not.
"""
import os
import re
import traceback

import renderdoc as rd

NAME_RE = re.compile(rb"[\\/]([A-Za-z0-9_\[\]@\.\-]+)\.[pcv]po\.pdb")


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
        actions = []

        def walk(items):
            for a in items:
                if a.flags & (rd.ActionFlags.Drawcall | rd.ActionFlags.Dispatch):
                    actions.append(a)
                walk(a.children)

        walk(controller.GetRootActions())
        cache = {}
        for a in actions:
            controller.SetFrameEvent(a.eventId, False)
            pipe = controller.GetPipelineState()
            compute = bool(a.flags & rd.ActionFlags.Dispatch)
            stages = [rd.ShaderStage.Compute] if compute else [rd.ShaderStage.Vertex, rd.ShaderStage.Pixel]
            parts = []
            for stage in stages:
                refl = pipe.GetShaderReflection(stage)
                if refl is None:
                    parts.append("none")
                    continue
                key = int(refl.resourceId)
                if key not in cache:
                    raw = bytes(refl.rawBytes)
                    m = NAME_RE.search(raw)
                    cache[key] = ((m.group(1).decode(errors="replace") if m else "?"),
                                  b"DynamicResolution" in raw)
                name, uses = cache[key]
                parts.append("%s%s" % ("+" if uses else "-", name))
            size = "-"
            if not compute:
                targets = [textures.get(int(d.resource)) for d in pipe.GetOutputTargets()]
                targets = [t for t in targets if t is not None]
                if targets:
                    size = "%dx%d" % (targets[0].width, targets[0].height)
            lines.append("%d %s %s %s" % (a.eventId, "CS" if compute else "draw", size, " ".join(parts)))
    except Exception:
        lines.append(traceback.format_exc())
    finally:
        controller.Shutdown()
        cap.Shutdown()
        with open(os.environ["ER_OUT"], "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
        os._exit(0)


main()
