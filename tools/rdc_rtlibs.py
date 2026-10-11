"""Dump the DXIL libraries of every ray tracing state object in a RenderDoc capture, for porting the game's shaders.

Run inside RenderDoc's embedded Python (qrenderdoc.exe --python tools/rdc_rtlibs.py) with:
  ER_RDC   path to the .rdc capture
  ER_OUT   output folder: lib_<chunk>_<n>.dxil per library, plus index.txt with each CreateStateObject /
           AddToStateObject call's subobject tree (names, types, sizes; byte buffers summarized)
Disassemble a library with the Windows SDK's dxc: dxc -dumpbin lib_<chunk>_<n>.dxil
"""
import os
import traceback

import renderdoc as rd


def walk(sf, obj, depth, lines, buffers):
    kind = obj.type.basetype
    if kind == rd.SDBasic.Buffer:
        index = obj.data.basic.u
        if index >= len(sf.buffers):
            lines.append("%s%s: buffer #%d not loaded" % ("  " * depth, obj.name, index))
            return
        data = bytes(sf.buffers[index])
        buffers.append((obj.name, data))
        lines.append("%s%s: buffer %d bytes, starts %r" % ("  " * depth, obj.name, len(data), data[:4]))
        return
    if obj.NumChildren() == 0:
        if kind == rd.SDBasic.String:
            value = obj.AsString()
        elif kind == rd.SDBasic.Float:
            value = obj.data.basic.d
        else:
            value = obj.data.basic.u
        lines.append("%s%s = %s" % ("  " * depth, obj.name, value))
        return
    lines.append("%s%s (%s)" % ("  " * depth, obj.name, obj.type.name))
    if depth > 12:
        return
    for i in range(obj.NumChildren()):
        walk(sf, obj.GetChild(i), depth + 1, lines, buffers)


try:
    out = os.environ["ER_OUT"]
    os.makedirs(out, exist_ok=True)
    cap = rd.OpenCaptureFile()
    cap.OpenFile(os.environ["ER_RDC"], "", None)
    sf = cap.GetStructuredData()  # the capture file's own copy keeps the byte buffers; no replay needed
    lines = []
    for ci, chunk in enumerate(sf.chunks):
        if "StateObject" not in chunk.name:
            continue
        lines.append("== chunk %d: %s" % (ci, chunk.name))
        buffers = []
        walk(sf, chunk, 1, lines, buffers)
        n = 0
        for name, data in buffers:
            if data[:4] == b"DXBC":  # DXIL containers keep the DXBC magic
                path = os.path.join(out, "lib_%d_%d.dxil" % (ci, n))
                with open(path, "wb") as f:
                    f.write(data)
                lines.append("  -> %s (%s)" % (os.path.basename(path), name))
                n += 1
    with open(os.path.join(out, "index.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    cap.Shutdown()
except Exception:
    with open(os.path.join(os.environ.get("ER_OUT", "."), "error.txt"), "w", encoding="utf-8") as f:
        f.write(traceback.format_exc())
os._exit(0)
