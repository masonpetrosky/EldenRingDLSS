"""Disassemble around instructions that use [reg + disp32] for given displacements (read-only).
usage: find_disp.py <exe> <hexdisp>[,<hexdisp>...] [--stores] [--max N]"""
import struct
import sys

import capstone

sys.path.insert(0, __import__("os").path.dirname(__file__))
from xref import Image  # noqa: E402

img = Image(sys.argv[1])
disps = [int(x, 16) for x in sys.argv[2].split(",")]
stores_only = "--stores" in sys.argv
limit = int(sys.argv[sys.argv.index("--max") + 1]) if "--max" in sys.argv else 30
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
shown = 0
for name, rva, vsize, raw, rsize, chars in img.code_sections():
    blob = img.data[raw:raw + rsize]
    for d in disps:
        pat = struct.pack("<I", d)
        i = blob.find(pat)
        while i != -1 and shown < limit:
            # find an instruction ending right after this disp32 (no immediate) or with a small immediate
            for back in range(2, 10):
                insns = list(md.disasm(blob[i - back:i + 8], img.base + rva + i - back))
                if insns and (insns[0].address + insns[0].size in (img.base + rva + i + 4, img.base + rva + i + 5,
                                                                    img.base + rva + i + 8)) \
                        and ("0x%x]" % d) in insns[0].op_str:
                    insn = insns[0]
                    is_store = insn.op_str.startswith(("dword ptr", "qword ptr", "xmmword ptr", "word ptr", "byte ptr"))
                    if stores_only and not is_store:
                        break
                    ctx = list(md.disasm(blob[i - back - 40:i + 60], img.base + rva + i - back - 40))
                    print("=== 0x%X: %s %s" % (insn.address, insn.mnemonic, insn.op_str))
                    for c in ctx:
                        if insn.address - 40 <= c.address <= insn.address + 40:
                            print("   %s 0x%X: %-8s %s" % ("=>" if c.address == insn.address else "  ", c.address,
                                                         c.mnemonic, c.op_str))
                    shown += 1
                    break
            i = blob.find(pat, i + 1)
