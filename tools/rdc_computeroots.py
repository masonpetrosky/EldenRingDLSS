"""Root parameter kinds of the compute dispatches in a capture: does the game bind compute arguments other than
descriptor tables (root constants or root descriptors), which an add-on dispatch in between would invalidate?

Run inside RenderDoc's embedded Python (qrenderdoc.exe --python tools/rdc_computeroots.py) with:
  ER_RDC   path to the .rdc capture
  ER_OUT   output text file
One line per dispatch: event, compute shader name and the root parameter kinds (T table, C constants, D root
descriptor), then a summary.
"""
import os
import re
import traceback

import renderdoc as rd

NAME_RE = re.compile(rb"[\\/]([A-Za-z0-9_\[\]@\.\-]+)\.cpo\.pdb")


def main():
    lines = []
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(os.environ["ER_RDC"], "", None) != rd.ResultCode.Succeeded:
        raise RuntimeError("cannot open capture")
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if result != rd.ResultCode.Succeeded:
        raise RuntimeError("cannot replay capture: %s" % result)
    try:
        dispatches = []

        def walk(actions):
            for a in actions:
                if a.flags & rd.ActionFlags.Dispatch:
                    dispatches.append(a)
                walk(a.children)

        walk(controller.GetRootActions())
        kinds = {}
        for a in dispatches:
            controller.SetFrameEvent(a.eventId, False)
            pipe = controller.GetPipelineState()
            refl = pipe.GetShaderReflection(rd.ShaderStage.Compute)
            m = NAME_RE.search(bytes(refl.rawBytes)) if refl is not None else None
            name = m.group(1).decode(errors="replace") if m else "?"
            d3d = controller.GetD3D12PipelineState()
            sig = []
            for param in d3d.rootSignature.parameters:
                if param.tableRanges:
                    sig.append("T")
                elif len(param.constants) > 0:
                    sig.append("C")
                else:
                    sig.append("D")  # root descriptor (CBV, SRV or UAV)
            text = "".join(sig)
            kinds[text] = kinds.get(text, 0) + 1
            lines.append("%d %s %s" % (a.eventId, name, text))
        lines.append("summary: " + ", ".join("%s x%d" % kv for kv in sorted(kinds.items(), key=lambda kv: -kv[1])))
    except Exception:
        lines.append(traceback.format_exc())
    finally:
        controller.Shutdown()
        cap.Shutdown()
        with open(os.environ["ER_OUT"], "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
        os._exit(0)


main()
