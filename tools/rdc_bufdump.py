"""Write raw bytes of a buffer range from a RenderDoc capture (ER_RDC, ER_OUT file, ER_BUF id, ER_OFF, ER_LEN)."""
import os
import renderdoc as rd
try:
    cap = rd.OpenCaptureFile()
    cap.OpenFile(os.environ["ER_RDC"], "", None)
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    controller.SetFrameEvent(int(os.environ.get("ER_EID", "0")) or controller.GetRootActions()[-1].eventId, True)
    target = int(os.environ["ER_BUF"])
    rid = next(b.resourceId for b in controller.GetBuffers() if int(b.resourceId) == target)
    data = controller.GetBufferData(rid, int(os.environ["ER_OFF"]), int(os.environ["ER_LEN"]))
    with open(os.environ["ER_OUT"], "wb") as f:
        f.write(bytes(data))
    controller.Shutdown(); cap.Shutdown()
finally:
    os._exit(0)
