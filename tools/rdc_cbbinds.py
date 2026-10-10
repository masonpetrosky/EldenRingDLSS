"""The constant buffers of the post-processing chain's draws: per draw and stage, each bound block's name, register,
buffer, byte offset and size, to see which draws share an allocation (dynamic-resolution experiments).

Run inside RenderDoc's embedded Python (qrenderdoc.exe --python tools/rdc_cbbinds.py) with:
  ER_RDC    path to the .rdc capture
  ER_OUT    output text file
  ER_FROM   optional: start at the first action whose shader names contain this text
            (default StencilAndMotionVectorPreprocess)
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
    start = os.environ.get("ER_FROM", "StencilAndMotionVectorPreprocess")
    try:
        actions = []

        def walk(items):
            for a in items:
                if a.flags & (rd.ActionFlags.Drawcall | rd.ActionFlags.Dispatch):
                    actions.append(a)
                walk(a.children)

        walk(controller.GetRootActions())
        names = {}
        started = False
        for a in actions:
            controller.SetFrameEvent(a.eventId, False)
            pipe = controller.GetPipelineState()
            compute = bool(a.flags & rd.ActionFlags.Dispatch)
            stages = [rd.ShaderStage.Compute] if compute else [rd.ShaderStage.Vertex, rd.ShaderStage.Pixel]
            parts = []
            for stage in stages:
                refl = pipe.GetShaderReflection(stage)
                if refl is None:
                    continue
                key = int(refl.resourceId)
                if key not in names:
                    m = NAME_RE.search(bytes(refl.rawBytes))
                    names[key] = m.group(1).decode(errors="replace") if m else "?"
                parts.append(names[key])
            if not started:
                if not any(start in p for p in parts):
                    continue
                started = True
            blocks = []
            for stage in stages:
                refl = pipe.GetShaderReflection(stage)
                if refl is None:
                    continue
                for slot, block in enumerate(refl.constantBlocks):
                    try:
                        desc = pipe.GetConstantBlock(stage, slot, 0).descriptor
                        blocks.append("%s:%s b%d buf%d+%d/%d" % (
                            str(stage).split(".")[-1][0], block.name, block.fixedBindNumber, int(desc.resource),
                            desc.byteOffset, desc.byteSize))
                    except Exception as exc:
                        blocks.append("%s:%s failed(%s)" % (str(stage).split(".")[-1][0], block.name, exc))
            lines.append("%d %s | %s" % (a.eventId, " ".join(parts), " ".join(blocks)))
    except Exception:
        lines.append(traceback.format_exc())
    finally:
        controller.Shutdown()
        cap.Shutdown()
        with open(os.environ["ER_OUT"], "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
        os._exit(0)


main()
