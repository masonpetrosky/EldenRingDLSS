"""Check which strings appear in raw shader bytecode at given events (ER_RDC, ER_OUT, ER_EIDS)."""
import os
import traceback
import renderdoc as rd
NEEDLES = [b"TempAA", b"SharpenFilter", b"StencilAndMotionVectorPreprocess", b"cbSceneParam", b"FC_MatrixProj",
           b"_Velo", b"SourceTex", b"g_MoveTex"]
try:
    cap = rd.OpenCaptureFile()
    cap.OpenFile(os.environ["ER_RDC"], "", None)
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    lines = []
    for eid in [int(x) for x in os.environ["ER_EIDS"].split(",")]:
        controller.SetFrameEvent(eid, True)
        pipe = controller.GetPipelineState()
        for stage in (rd.ShaderStage.Vertex, rd.ShaderStage.Pixel):
            refl = pipe.GetShaderReflection(stage)
            if refl is None:
                continue
            raw = bytes(refl.rawBytes)
            found = [n.decode() for n in NEEDLES if n in raw]
            cbs = ["%s@b%d" % (c.name, c.fixedBindNumber) for c in refl.constantBlocks]
            lines.append("%d %s: %d bytes, starts %r, found %s, cbuffers %s" % (eid, stage, len(raw), raw[:4], found, cbs))
    with open(os.environ["ER_OUT"], "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    controller.Shutdown(); cap.Shutdown()
except Exception:
    with open(os.environ["ER_OUT"], "w", encoding="utf-8") as f:
        f.write(traceback.format_exc())
finally:
    os._exit(0)
