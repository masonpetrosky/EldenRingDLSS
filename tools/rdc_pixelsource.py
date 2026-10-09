"""Which G-buffer draw produced each pixel of a grid: RenderDoc pixel history on the main depth target.

Run inside RenderDoc's embedded Python (qrenderdoc.exe --python tools/rdc_pixelsource.py) with:
  ER_RDC    path to the .rdc capture
  ER_NAMES  the capture's tools/rdc_drawnames.py listing (to find the G-buffer draws and their shader names)
  ER_OUT    output text file: one line per grid pixel "x y event vertex-shader" (event 0: no G-buffer draw)
  ER_IMG    optional: PNG of the G-buffer's first target (albedo) at the end of the pass, to see the view
  ER_STEP   grid spacing in pixels (default 120)
The G-buffer draws are the listing's 3840x2160 draws with five render targets; a pixel's source is the last of them
that passed the depth test there.
"""
import os
import traceback

import renderdoc as rd


def main():
    lines = []
    names = {}
    for line in open(os.environ["ER_NAMES"], encoding="utf-8"):
        parts = line.split()
        if len(parts) >= 7 and parts[4] == "rt=5" and parts[5] == "3840x2160":
            names[int(parts[0])] = parts[2]
    last = max(names)
    step = int(os.environ.get("ER_STEP", "120"))
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(os.environ["ER_RDC"], "", None) != rd.ResultCode.Succeeded:
        raise RuntimeError("cannot open capture")
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if result != rd.ResultCode.Succeeded:
        raise RuntimeError("cannot replay capture: %s" % result)
    try:
        controller.SetFrameEvent(last, True)
        pipe = controller.GetPipelineState()
        depth = pipe.GetDepthTarget().resource
        if os.environ.get("ER_IMG"):
            save = rd.TextureSave()
            save.resourceId = pipe.GetOutputTargets()[0].resource
            save.destType = rd.FileType.PNG
            controller.SaveTexture(save, os.environ["ER_IMG"])
        for y in range(step // 2, 2160, step):
            for x in range(step // 2, 3840, step):
                mods = controller.PixelHistory(depth, x, y, rd.Subresource(0, 0, 0), rd.CompType.Typeless)
                hits = [m.eventId for m in mods if m.eventId in names and m.Passed()]
                eid = hits[-1] if hits else 0
                lines.append("%d %d %d %s" % (x, y, eid, names.get(eid, "-")))
    except Exception:
        lines.append(traceback.format_exc())
    finally:
        controller.Shutdown()
        cap.Shutdown()
        with open(os.environ["ER_OUT"], "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
        os._exit(0)


main()
