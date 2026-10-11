"""Saves a capture's embedded thumbnail (RenderDoc's in-app capture preview) to check what was captured.

Run inside RenderDoc's embedded Python (qrenderdoc.exe --python tools/rdc_thumb.py) with:
  ER_RDC   path to the .rdc capture
  ER_OUT   output image (.jpg or .png, RenderDoc's own encoding of the thumbnail)
"""
import os
import traceback

import renderdoc as rd

try:
    cap = rd.OpenCaptureFile()
    cap.OpenFile(os.environ["ER_RDC"], "", None)
    thumb = cap.GetThumbnail(rd.FileType.JPG, 1024)
    with open(os.environ["ER_OUT"], "wb") as f:
        f.write(bytes(thumb.data))
    cap.Shutdown()
except Exception:
    with open(os.environ["ER_OUT"] + ".error.txt", "w", encoding="utf-8") as f:
        f.write(traceback.format_exc())
os._exit(0)
