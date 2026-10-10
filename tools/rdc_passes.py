"""The post-processing chain of a capture, one line per draw or dispatch: shader names, viewport, targets, inputs
and every constant whose name mentions a screen percentage or dynamic resolution (dynamic-resolution experiments).

Run inside RenderDoc's embedded Python (qrenderdoc.exe --python tools/rdc_passes.py) with:
  ER_RDC    path to the .rdc capture
  ER_OUT    output text file
  ER_FROM   optional: start at the first action whose shader names contain this text
            (default StencilAndMotionVectorPreprocess, the pass before the TAA); "all" for the whole frame
Textures are written as tex<id>:<width>x<height>; the same id is the same texture within a capture.
"""
import os
import re
import traceback

import renderdoc as rd

NAME_RE = re.compile(rb"[\\/]([A-Za-z0-9_\[\]@\.\-]+)\.[pcv]po\.pdb")
WATCH = re.compile(r"ScreenPercentage|DynamicResolution|texSizeReciprocal|ScreenSize", re.I)


def values(var):
    if var.members:
        out = []
        for m in var.members:
            out.extend(values(m))
        return out
    count = var.rows * var.columns
    return ["%.4g" % var.value.f32v[i] for i in range(count)]


def flat(var, prefix=""):
    name = prefix + var.name
    if var.members and not var.rows:
        for m in var.members:
            yield from flat(m, name + ".")
    else:
        yield name, var


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
        textures = {int(t.resourceId): t for t in controller.GetTextures()}
        names = {int(r.resourceId): r.name for r in controller.GetResources()}

        def tex(rid):
            t = textures.get(int(rid))
            if t is None:
                return "res%d" % int(rid)
            label = names.get(int(rid), "")
            label = "" if label.startswith(("Texture", "Resource")) else "'%s'" % label
            return "tex%d:%dx%d%s" % (int(rid), t.width, t.height, label)

        actions = []

        def walk(items):
            for a in items:
                if a.flags & (rd.ActionFlags.Drawcall | rd.ActionFlags.Dispatch):
                    actions.append(a)
                walk(a.children)

        walk(controller.GetRootActions())
        shader_names = {}
        started = start == "all"
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
                if key not in shader_names:
                    m = NAME_RE.search(bytes(refl.rawBytes))
                    shader_names[key] = m.group(1).decode(errors="replace") if m else "?"
                parts.append(shader_names[key])
            if not started:
                if not any(start in p for p in parts):
                    continue
                started = True

            line = ["%d %s %s" % (a.eventId, "CS" if compute else "draw", " ".join(parts))]
            if not compute:
                try:
                    vp = pipe.GetViewport(0)
                    line.append("vp %g,%g %gx%g" % (vp.x, vp.y, vp.width, vp.height))
                except Exception:
                    line.append("vp ?")
                rts = [tex(d.resource) for d in pipe.GetOutputTargets() if d.resource != rd.ResourceId.Null()]
                line.append("RT " + (",".join(rts) if rts else "-"))
                depth = pipe.GetDepthTarget()
                if depth.resource != rd.ResourceId.Null():
                    line.append("DS " + tex(depth.resource))
            inputs = []
            for stage in stages:
                refl = pipe.GetShaderReflection(stage)
                if refl is None:
                    continue
                for used in pipe.GetReadOnlyResources(stage, True):
                    idx = used.access.index
                    label = refl.readOnlyResources[idx].name if 0 <= idx < len(refl.readOnlyResources) else "#%d" % idx
                    if int(used.descriptor.resource) in textures:
                        inputs.append("%s=%s" % (label, tex(used.descriptor.resource)))
                for used in pipe.GetReadWriteResources(stage, True):
                    idx = used.access.index
                    label = refl.readWriteResources[idx].name if 0 <= idx < len(refl.readWriteResources) else "#%d" % idx
                    if int(used.descriptor.resource) in textures:
                        inputs.append("UAV %s=%s" % (label, tex(used.descriptor.resource)))
            if inputs:
                line.append("in " + " ".join(inputs))
            consts = []
            for stage in stages:
                refl = pipe.GetShaderReflection(stage)
                if refl is None:
                    continue
                shader = pipe.GetShader(stage)
                entry = pipe.GetShaderEntryPoint(stage)
                pipeline_obj = pipe.GetComputePipelineObject() if compute else pipe.GetGraphicsPipelineObject()
                for slot, block in enumerate(refl.constantBlocks):
                    if not any(WATCH.search(v.name) for v in block.variables) and not any(
                            WATCH.search(m.name) for v in block.variables for m in v.type.members):
                        continue
                    try:
                        desc = pipe.GetConstantBlock(stage, slot, 0).descriptor
                        contents = controller.GetCBufferVariableContents(
                            pipeline_obj, shader, stage, entry, slot, desc.resource, desc.byteOffset,
                            desc.byteSize)
                        for var in contents:
                            for name, leaf in flat(var):
                                if WATCH.search(name):
                                    consts.append("%s=%s" % (name, "/".join(values(leaf))))
                    except Exception as exc:
                        consts.append("%s:failed(%s)" % (block.name, exc))
            if consts:
                line.append("c " + " ".join(sorted(set(consts))))
            lines.append(" | ".join(line))
    except Exception:
        lines.append(traceback.format_exc())
    finally:
        controller.Shutdown()
        cap.Shutdown()
        with open(os.environ["ER_OUT"], "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
        os._exit(0)


main()
