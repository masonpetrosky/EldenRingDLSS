"""List every GPU action of a RenderDoc capture, for the ray tracing work.

Run inside RenderDoc's embedded Python (qrenderdoc.exe --python tools/rdc_rt.py) with:
  ER_RDC   path to the .rdc capture
  ER_OUT   output text file
One line per action (draw, dispatch, DispatchRays, acceleration-structure build, copy, clear): event, GPU time in
microseconds (only with ER_TIMES=1; RenderDoc's counters returned nothing in headless replay here), the call, its shader names
(vertex/pixel or compute), dispatch size or render target size; DispatchRays and acceleration-structure builds also
get their parameters. Ends with totals per call type.
"""
import collections
import os
import re
import traceback

import renderdoc as rd

NAME_RE = re.compile(rb"[\\/]([A-Za-z0-9_\[\]@\.\-]+)\.[pcv]po\.pdb")


def debug_name(refl):
    if refl is None:
        return "-"
    m = NAME_RE.search(bytes(refl.rawBytes))
    return m.group(1).decode(errors="replace") if m else "?"


def flatten(obj, prefix, out, depth=0):
    if depth > 6 or len(out) > 40:
        return
    if obj.NumChildren() == 0:
        if obj.type.basetype in (rd.SDBasic.UnsignedInteger, rd.SDBasic.SignedInteger):
            out.append("%s=%d" % (prefix, obj.AsInt()))
        elif obj.type.basetype == rd.SDBasic.Enum:
            out.append("%s=%s" % (prefix, obj.AsString()))
        elif obj.type.basetype == rd.SDBasic.Resource:
            out.append("%s=%s" % (prefix, obj.AsResourceId()))
        return
    for i in range(obj.NumChildren()):
        child = obj.GetChild(i)
        flatten(child, (prefix + "." if prefix else "") + child.name, out, depth + 1)


def params(sf, action):
    out = []
    for ev in action.events:
        if ev.eventId == action.eventId:
            flatten(sf.chunks[ev.chunkIndex], "", out)
    return " ".join(out)


def main():
    lines = []
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(os.environ["ER_RDC"], "", None) != rd.ResultCode.Succeeded:
        raise RuntimeError("cannot open capture")
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if result != rd.ResultCode.Succeeded:
        raise RuntimeError("cannot replay capture: %s" % result)
    try:
        sf = controller.GetStructuredFile()
        textures = {int(t.resourceId): t for t in controller.GetTextures()}
        actions = []

        def walk(items):
            for a in items:
                if not a.children:
                    actions.append(a)
                walk(a.children)

        walk(controller.GetRootActions())
        durations = {}
        if os.environ.get("ER_TIMES") == "1":  # FetchCounters returned nothing here, and shader names were lost after it
            for r in controller.FetchCounters([rd.GPUCounter.EventGPUDuration]):
                durations[r.eventId] = r.value.d * 1e6
        totals = collections.Counter()
        counts = collections.Counter()
        by_shader = collections.Counter()
        for a in actions:
            name = a.GetName(sf)
            call = name.split("(")[0]
            us = durations.get(a.eventId, 0.0)
            detail = ""
            if a.flags & (rd.ActionFlags.Drawcall | rd.ActionFlags.Dispatch):
                controller.SetFrameEvent(a.eventId, True)
                pipe = controller.GetPipelineState()
                if a.flags & rd.ActionFlags.Dispatch:
                    cs = debug_name(controller.GetD3D12PipelineState().computeShader.reflection)
                    detail = "cs=%s groups=%s" % (cs, "x".join(str(d) for d in a.dispatchDimension))
                    by_shader[cs] += us
                else:
                    vs = debug_name(pipe.GetShaderReflection(rd.ShaderStage.Vertex))
                    ps = debug_name(pipe.GetShaderReflection(rd.ShaderStage.Pixel))
                    targets = [textures.get(int(d.resource)) for d in pipe.GetOutputTargets()]
                    targets = [t for t in targets if t is not None]
                    size = "%dx%d" % (targets[0].width, targets[0].height) if targets else "-"
                    vp = pipe.GetViewport(0)
                    detail = "vs=%s ps=%s rt=%s vp=%dx%d" % (vs, ps, size, vp.width, vp.height)
                    by_shader[ps] += us
            if "DispatchRays" in call or "BuildRaytracing" in call:
                detail = params(sf, a)
            totals[call] += us
            counts[call] += 1
            lines.append("%6d %9.1f %s %s" % (a.eventId, us, name[:120], detail))
        lines.append("")
        lines.append("== totals per call (us, count)")
        for call, us in totals.most_common():
            lines.append("%10.1f %6d %s" % (us, counts[call], call))
        lines.append("== total %.1f us" % sum(totals.values()))
        lines.append("== most expensive shaders (us)")
        for shader, us in by_shader.most_common(60):
            lines.append("%10.1f %s" % (us, shader))
    except Exception:
        lines.append(traceback.format_exc())
    finally:
        controller.Shutdown()
        cap.Shutdown()
        with open(os.environ["ER_OUT"], "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
        os._exit(0)


main()
