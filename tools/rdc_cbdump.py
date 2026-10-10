"""Every constant of the post-processing chain's draws, with register and byte offset, one line per variable, to diff
two captures (dynamic-resolution experiments: which constants change with the render scale).

Run inside RenderDoc's embedded Python (qrenderdoc.exe --python tools/rdc_cbdump.py) with:
  ER_RDC    path to the .rdc capture
  ER_OUT    output text file
  ER_FROM   optional: start at the first action whose shader names contain this text
            (default StencilAndMotionVectorPreprocess)
Lines: <index> <shader names> | <stage> <block> b<register> +<byte offset> <name> = <values>
(<index> counts the actions from the start, so the same pass gets the same index in two similar captures.)
"""
import os
import re
import traceback

import renderdoc as rd

NAME_RE = re.compile(rb"[\\/]([A-Za-z0-9_\[\]@\.\-]+)\.[pcv]po\.pdb")


def leaves(var, offset, prefix=""):
    """(byte offset, name, values) for every leaf of a constant, offsets relative to the block."""
    name = prefix + var.name
    if var.members:
        for m in var.members:
            # Struct members carry their own offsets; array elements follow each other.
            yield from leaves(m, offset, name + ".")
        return
    count = var.rows * var.columns
    if var.type in (rd.VarType.SInt, rd.VarType.UInt, rd.VarType.Bool):
        vals = [str(var.value.s32v[i]) for i in range(count)]
    else:
        vals = ["%.6g" % var.value.f32v[i] for i in range(count)]
    yield offset, name, vals


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
        index = -1
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
            if index < 0 and not any(start in p for p in parts):
                continue
            index += 1
            head = "%03d %s" % (index, " ".join(parts))
            pipeline_obj = pipe.GetComputePipelineObject() if compute else pipe.GetGraphicsPipelineObject()
            for stage in stages:
                refl = pipe.GetShaderReflection(stage)
                if refl is None:
                    continue
                shader = pipe.GetShader(stage)
                entry = pipe.GetShaderEntryPoint(stage)
                st = str(stage).split(".")[-1][0]
                for slot, block in enumerate(refl.constantBlocks):
                    try:
                        desc = pipe.GetConstantBlock(stage, slot, 0).descriptor
                        contents = controller.GetCBufferVariableContents(
                            pipeline_obj, shader, stage, entry, slot, desc.resource, desc.byteOffset,
                            desc.byteSize)
                        offsets = {v.name: v.byteOffset for v in block.variables}
                        for var in contents:
                            for off, name, vals in leaves(var, offsets.get(var.name, -1)):
                                lines.append("%s | %s %s b%d +%d %s = %s" % (
                                    head, st, block.name, block.fixedBindNumber, off, name, "/".join(vals)))
                    except Exception as exc:
                        lines.append("%s | %s %s failed (%s)" % (head, st, block.name, exc))
    except Exception:
        lines.append(traceback.format_exc())
    finally:
        controller.Shutdown()
        cap.Shutdown()
        with open(os.environ["ER_OUT"], "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
        os._exit(0)


main()
