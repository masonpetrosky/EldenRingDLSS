"""List the skinned draws of a RenderDoc capture with their instancing: which characters each draw covers and where
their bone palettes are (NPC motion vectors).

Run inside RenderDoc's embedded Python (qrenderdoc.exe --python tools/rdc_skin.py) with:
  ER_RDC   path to the .rdc capture
  ER_OUT   output text file
  ER_MATCH optional: only draws whose vertex shader name contains one of these comma-separated strings
           (default "_Skin")
Per draw: event, shader names, render target count, indices x instances, first index, base vertex, first instance,
vertex/index buffer (resource + offset), the constant buffer windows (b4 cbInstanceData, b5 cbObjMatrix, b6
cbPrevObjMatrix, b10 cbInstanceIdxParam, b12 cbClothCancelObjMatrix), VC_InstanceIdxOffset and, for every instance,
its g_InstanceIndexBuffer entry, matricesData, the four offsets behind it, the world position (mWorld translation) and
the translation of its first bone.
"""
import os
import re
import struct
import traceback

import renderdoc as rd

NAME_RE = re.compile(rb"[\\/]([A-Za-z0-9_\[\]@\.\-]+)\.[pcv]po\.pdb")
MATCH = [m for m in os.environ.get("ER_MATCH", "_Skin").split(",") if m]
INSTANCE_SIZE = 13 * 16  # PerInstanceData: 13 registers (the shaders index it as idx * 13 + 4 for matricesData)
BLOCKS = ("cbInstanceData", "cbObjMatrix", "cbPrevObjMatrix", "cbInstanceIdxParam", "cbClothCancelObjMatrix")


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
        for a in draws:
            controller.SetFrameEvent(a.eventId, False)
            pipe = controller.GetPipelineState()
            vrefl = pipe.GetShaderReflection(rd.ShaderStage.Vertex)
            vs = debug_name(vrefl)
            if not any(m in vs for m in MATCH):
                continue
            ps = debug_name(pipe.GetShaderReflection(rd.ShaderStage.Pixel))
            rts = sum(1 for d in pipe.GetOutputTargets() if d.resource != rd.ResourceId.Null())
            vbs = pipe.GetVBuffers()
            ib = pipe.GetIBuffer()
            vb = " ".join("vb%d=%d+%d/%d" % (i, int(v.resourceId), v.byteOffset, v.byteStride)
                          for i, v in enumerate(vbs) if v.resourceId != rd.ResourceId.Null())
            lines.append("%d %s %s rt=%d n=%dx%d first=%d base=%d inst0=%d %s ib=%d+%d" % (
                a.eventId, vs, ps, rts, a.numIndices, a.numInstances, a.indexOffset, a.baseVertex,
                a.instanceOffset, vb, int(ib.resourceId), ib.byteOffset))
            windows = {}
            for slot, block in enumerate(vrefl.constantBlocks):
                if block.name in BLOCKS:
                    d = pipe.GetConstantBlock(rd.ShaderStage.Vertex, slot, 0).descriptor
                    windows[block.name] = (d.resource, d.byteOffset, d.byteSize)
            lines.append("    " + " ".join("%s=%d+%d/%d" % (n, int(w[0]), w[1], w[2])
                                           for n, w in windows.items()))
            index_buffer = None
            for used in pipe.GetReadOnlyResources(rd.ShaderStage.Vertex, True):
                idx = used.access.index
                if 0 <= idx < len(vrefl.readOnlyResources) and \
                        vrefl.readOnlyResources[idx].name == "g_InstanceIndexBuffer":
                    index_buffer = (used.descriptor.resource, used.descriptor.byteOffset)
            if "cbInstanceData" not in windows or "cbInstanceIdxParam" not in windows or index_buffer is None:
                continue
            res, off, _ = windows["cbInstanceIdxParam"]
            idx_offset = struct.unpack("<i", bytes(controller.GetBufferData(res, off, 4)))[0]
            inst_res, inst_off, inst_size = windows["cbInstanceData"]
            instances = bytes(controller.GetBufferData(inst_res, inst_off, inst_size))
            mat = windows.get("cbObjMatrix")
            for i in range(a.numInstances):
                entry = struct.unpack("<i", bytes(controller.GetBufferData(
                    index_buffer[0], index_buffer[1] + 4 * (idx_offset + i), 4)))[0]
                base = entry * INSTANCE_SIZE
                if base + INSTANCE_SIZE > len(instances):
                    lines.append("    i%d entry=%d outside cbInstanceData" % (i, entry))
                    continue
                world = struct.unpack_from("<12f", instances, base)
                ints = struct.unpack_from("<8i", instances, base + 64)
                bone = ""
                if mat is not None:
                    raw = bytes(controller.GetBufferData(mat[0], mat[1] + ints[0] * 48, 48))
                    m = struct.unpack("<12f", raw)
                    bone = " bone0=(%.3f %.3f %.3f)" % (m[3], m[7], m[11])
                lines.append("    i%d entry=%d idxoff=%d matricesData=%d next=%s pos=(%.3f %.3f %.3f)%s" % (
                    i, entry, idx_offset, ints[0], ",".join(str(v) for v in ints[1:4]),
                    world[3], world[7], world[11], bone))
    except Exception:
        lines.append(traceback.format_exc())
    finally:
        controller.Shutdown()
        cap.Shutdown()
        with open(os.environ["ER_OUT"], "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
        os._exit(0)


main()
