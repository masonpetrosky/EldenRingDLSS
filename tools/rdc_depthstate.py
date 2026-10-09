"""Print the depth-stencil and rasterizer state and the pixel shader name of chosen draws from a RenderDoc capture.

Run inside RenderDoc's embedded Python (qrenderdoc.exe --python tools/rdc_depthstate.py) with:
  ER_RDC   path to the .rdc capture
  ER_OUT   output text file
  ER_EIDS  comma-separated event IDs
"""
import os
import traceback

import renderdoc as rd

CAPTURE = os.environ["ER_RDC"]
OUT = os.environ["ER_OUT"]
EIDS = [int(x) for x in os.environ["ER_EIDS"].split(",") if x.strip()]


def shader_name(controller, refl):
    if refl is None:
        return "none"
    try:
        info = refl.debugInfo
        if info.files:
            return info.files[0].filename
    except Exception:
        pass
    return str(refl.resourceId)


def main():
    lines = []
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(CAPTURE, "", None) != rd.ResultCode.Succeeded:
        raise RuntimeError("cannot open capture")
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if result != rd.ResultCode.Succeeded:
        raise RuntimeError("cannot replay capture: %s" % result)
    try:
        for eid in EIDS:
            controller.SetFrameEvent(eid, True)
            d3d = controller.GetD3D12PipelineState()
            ds = d3d.outputMerger.depthStencilState
            ra = d3d.rasterizer.state
            pipe = controller.GetPipelineState()
            ps = pipe.GetShaderReflection(rd.ShaderStage.Pixel)
            lines.append("== event %d" % eid)
            lines.append("  pixel shader: %s" % shader_name(controller, ps))
            lines.append("  depth: enable %s, writes %s, function %s, bounds %s" % (
                ds.depthEnable, ds.depthWrites, ds.depthFunction, ds.depthBoundsEnable))
            lines.append("  stencil: enable %s, read 0x%02x, write 0x%02x, ref 0x%02x" % (
                ds.stencilEnable, ds.frontFace.compareMask, ds.frontFace.writeMask, ds.frontFace.reference))
            lines.append("  front: func %s, pass %s, fail %s, depth fail %s" % (
                ds.frontFace.function, ds.frontFace.passOperation, ds.frontFace.failOperation,
                ds.frontFace.depthFailOperation))
            lines.append("  rasterizer: cull %s, depth bias %s, slope %s, clamp %s, clip %s" % (
                ra.cullMode, ra.depthBias, ra.slopeScaledDepthBias, ra.depthBiasClamp, ra.depthClip))
    except Exception:
        lines.append(traceback.format_exc())
    finally:
        controller.Shutdown()
        cap.Shutdown()
        with open(OUT, "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
        os._exit(0)


main()
