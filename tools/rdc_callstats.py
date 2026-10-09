"""Count API calls by name across a RenderDoc capture (ER_RDC, ER_OUT file)."""
import collections
import os
import renderdoc as rd
try:
    cap = rd.OpenCaptureFile()
    cap.OpenFile(os.environ["ER_RDC"], "", None)
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    sf = controller.GetStructuredFile()
    counts = collections.Counter(chunk.name for chunk in sf.chunks)
    with open(os.environ["ER_OUT"], "w", encoding="utf-8") as f:
        for name, n in counts.most_common():
            f.write("%7d  %s\n" % (n, name))
    controller.Shutdown(); cap.Shutdown()
finally:
    os._exit(0)
