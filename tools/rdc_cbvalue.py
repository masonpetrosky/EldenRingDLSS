"""Print one constant-buffer variable of the vertex shader for every draw that uses chosen vertex shaders.

Run inside RenderDoc's embedded Python (qrenderdoc.exe --python tools/rdc_cbvalue.py) with:
  ER_RDC   path to the .rdc capture
  ER_OUT   output text file
  ER_VS    comma-separated vertex shader resource ids (e.g. 146073,146102)
  ER_VAR   variable name (e.g. VC_InstanceIdxOffset)
"""
import os
import traceback

import renderdoc as rd

CAPTURE = os.environ["ER_RDC"]
OUT = os.environ["ER_OUT"]
SHADERS = {int(x) for x in os.environ["ER_VS"].split(",") if x.strip()}
VAR = os.environ["ER_VAR"]


def find(variables, name):
    for v in variables:
        if v.name == name:
            return v
        found = find(v.members, name) if v.members else None
        if found is not None:
            return found
    return None


def main():
    lines = []
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(CAPTURE, "", None) != rd.ResultCode.Succeeded:
        raise RuntimeError("cannot open capture")
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if result != rd.ResultCode.Succeeded:
        raise RuntimeError("cannot replay capture: %s" % result)
    try:
        actions = []

        def walk(items):
            for a in items:
                if a.flags & rd.ActionFlags.Drawcall:
                    actions.append(a)
                walk(a.children)

        walk(controller.GetRootActions())
        for a in actions:
            controller.SetFrameEvent(a.eventId, False)
            pipe = controller.GetPipelineState()
            vs = pipe.GetShader(rd.ShaderStage.Vertex)
            if int(vs) not in SHADERS:
                continue
            refl = pipe.GetShaderReflection(rd.ShaderStage.Vertex)
            value = None
            for i, block in enumerate(refl.constantBlocks):
                bind = pipe.GetConstantBlock(rd.ShaderStage.Vertex, i, 0)
                variables = controller.GetCBufferVariableContents(
                    pipe.GetGraphicsPipelineObject(), refl.resourceId, rd.ShaderStage.Vertex,
                    pipe.GetShaderEntryPoint(rd.ShaderStage.Vertex), i, bind.descriptor.resource,
                    bind.descriptor.byteOffset, bind.descriptor.byteSize)
                v = find(variables, VAR)
                if v is not None:
                    value = v.value.s32v[0] if v.type == rd.VarType.SInt else v.value.f32v[0]
                    break
            lines.append("%d vs=%d instances=%d %s=%s" % (a.eventId, int(vs), a.numInstances, VAR, value))
    except Exception:
        lines.append(traceback.format_exc())
    finally:
        controller.Shutdown()
        cap.Shutdown()
        with open(OUT, "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
        os._exit(0)


main()
