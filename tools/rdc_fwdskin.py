"""Per skinned forward draw (vertex shader *_Fwd_Skin@*), plus the first G-buffer draw of each skinned material for
comparison: targets, depth-stencil and blend state, topology, extra stages, root signature and the vertex stage's
constant buffer registers.

Run inside RenderDoc's embedded Python (qrenderdoc.exe --python tools/rdc_fwdskin.py) with ER_RDC and ER_OUT set.
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


def safe(fn, default="?"):
    try:
        return fn()
    except Exception as e:  # noqa: BLE001
        return "%s(%s)" % (default, type(e).__name__)


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

        def tex(resource_id):
            t = textures.get(int(resource_id))
            if t is None:
                return "%d(?)" % int(resource_id)
            return "%d %dx%d %s" % (int(resource_id), t.width, t.height, t.format.Name())

        draws = []

        def walk(actions):
            for a in actions:
                if a.flags & rd.ActionFlags.Drawcall:
                    draws.append(a)
                walk(a.children)

        walk(controller.GetRootActions())
        summary = Counter()
        gbuf_seen = set()
        for a in draws:
            controller.SetFrameEvent(a.eventId, False)
            pipe = controller.GetPipelineState()
            vrefl = pipe.GetShaderReflection(rd.ShaderStage.Vertex)
            vs = debug_name(vrefl)
            if "_Skin" not in vs:
                continue
            material = vs.split("@")[0]
            forward = "_Fwd" in vs
            if not forward:
                if "_Gbuf" not in vs or material in gbuf_seen:
                    continue
                gbuf_seen.add(material)
            ps = debug_name(pipe.GetShaderReflection(rd.ShaderStage.Pixel))
            d3d = controller.GetD3D12PipelineState()
            om = d3d.outputMerger
            ds = om.depthStencilState
            ff = ds.frontFace
            rts = [d.resource for d in pipe.GetOutputTargets() if d.resource != rd.ResourceId.Null()]
            depth = safe(lambda: pipe.GetDepthTarget().resource)
            blend = safe(lambda: om.blendState.blends[0])
            blend_text = safe(lambda: "blend %d %s/%s %s mask %x" % (
                blend.enabled, str(blend.colorBlend.source).split(".")[-1],
                str(blend.colorBlend.destination).split(".")[-1], str(blend.colorBlend.operation).split(".")[-1],
                blend.writeMask))
            others = [str(s).split(".")[-1] for s in (rd.ShaderStage.Hull, rd.ShaderStage.Domain,
                                                       rd.ShaderStage.Geometry)
                      if pipe.GetShader(s) != rd.ResourceId.Null()]
            topology = safe(lambda: str(pipe.GetPrimitiveTopology()).split(".")[-1])
            root = safe(lambda: int(d3d.rootSignatureResourceId))
            cbs = " ".join("%s=b%d" % (b.name, b.fixedBindNumber) for b in vrefl.constantBlocks)
            srvs = " ".join("%s=t%d" % (r.name, r.fixedBindNumber) for r in vrefl.readOnlyResources)
            depth_text = "depth %d write %d func %s stencil %d pass %s func %s ref %02x write %02x" % (
                ds.depthEnable, ds.depthWrites, str(ds.depthFunction).split(".")[-1], ds.stencilEnable,
                str(ff.passOperation).split(".")[-1], str(ff.function).split(".")[-1], ff.reference, ff.writeMask)
            lines.append("%d %s | %s | n=%dx%d | rts %d: %s | dsv %s | %s | %s | %s %s | root %s" % (
                a.eventId, vs, ps, a.numIndices, a.numInstances, len(rts), ", ".join(tex(r) for r in rts),
                tex(depth) if not isinstance(depth, str) else depth, depth_text, blend_text, topology,
                ("stages " + ",".join(others)) if others else "", root))
            lines.append("    vs cbs: %s" % cbs)
            lines.append("    vs srvs: %s" % srvs)
            if forward:
                rt0 = textures.get(int(rts[0])) if rts else None
                summary[(material, ps.split("@")[0], len(rts), "%dx%d" % (rt0.width, rt0.height) if rt0 else "-",
                         depth_text, blend_text, topology, ",".join(others), root)] += 1
        lines.append("")
        lines.append("summary of forward draws (material, PS, RT count, RT0 size, depth-stencil, blend, topology, "
                     "other stages, root signature): draws")
        for key, n in sorted(summary.items(), key=lambda kv: str(kv[0])):
            lines.append("%s: %d" % (key, n))
    except Exception:
        lines.append(traceback.format_exc())
    finally:
        controller.Shutdown()
        cap.Shutdown()
        with open(os.environ["ER_OUT"], "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
        os._exit(0)


main()
