"""Find code that references several cbSceneParam offsets (disp32) within a short span (read-only)."""
import struct
import sys

import capstone

sys.path.insert(0, __import__("os").path.dirname(__file__))
from xref import Image  # noqa: E402

img = Image(sys.argv[1])
offsets = [0x550, 0x560, 0x570, 0x58C]
span = int(sys.argv[2]) if len(sys.argv) > 2 else 400
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
for name, rva, vsize, raw, rsize, chars in img.code_sections():
    blob = img.data[raw:raw + rsize]
    anchor = struct.pack("<I", 0x58C)
    i = blob.find(anchor)
    while i != -1:
        lo, hi = max(0, i - span), min(len(blob), i + span)
        if all(blob.find(struct.pack("<I", o), lo, hi) != -1 for o in offsets):
            va = img.base + rva + i
            print("=== candidate near 0x%X" % va)
            start = img.base + rva + lo
            for insn in md.disasm(blob[lo:hi], start):
                if any(("0x%x]" % o) in insn.op_str for o in offsets) or "xmm" in insn.op_str and "0x5" in insn.op_str:
                    print("   0x%X: %-8s %s" % (insn.address, insn.mnemonic, insn.op_str))
        i = blob.find(anchor, i + 1)
