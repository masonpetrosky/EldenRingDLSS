"""Find code that stores to [reg + A] and [reg + B] (disp32) close together, and disassemble it.
usage: find_store.py <exe> <hexA> <hexB> [window]"""
import struct
import sys

import capstone

sys.path.insert(0, __import__("os").path.dirname(__file__))
from xref import Image  # noqa: E402

img = Image(sys.argv[1])
a, b = int(sys.argv[2], 16), int(sys.argv[3], 16)
window = int(sys.argv[4]) if len(sys.argv) > 4 else 96
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
pa, pb = struct.pack("<I", a), struct.pack("<I", b)
found = []
for name, rva, vsize, raw, rsize, chars in img.code_sections():
    blob = img.data[raw:raw + rsize]
    i = blob.find(pa)
    while i != -1:
        lo, hi = max(0, i - window), i + window
        if blob.find(pb, lo, hi) != -1:
            found.append(img.base + rva + i)
        i = blob.find(pa, i + 1)
print("%d site(s) with 0x%X and 0x%X within %d bytes" % (len(found), a, b, window))
for va in found[:12]:
    print("--- 0x%X" % va)
    start = va - 48
    insns = list(md.disasm(img.data[img.va_to_off(start):img.va_to_off(start) + 220], start))
    # resync: keep only instructions after the first one that covers va-48+... just print a window
    for insn in insns[:36]:
        mark = "=>" if insn.address <= va < insn.address + insn.size else "  "
        print("%s 0x%X: %-8s %s" % (mark, insn.address, insn.mnemonic, insn.op_str))
