"""Heuristic search for code that reads LoadBalancerParam's dynamic resolution bytes (offsets 0x30 and 0x31)."""
import struct
import sys

import capstone

sys.path.insert(0, __import__("os").path.dirname(__file__))
from xref import Image  # noqa: E402

img = Image(sys.argv[1])
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.detail = False
hits = []
for name, rva, vsize, raw, rsize, chars in img.code_sections():
    blob = img.data[raw:raw + rsize]
    # movzx r32, byte ptr [base + disp8]: (REX)? 0F B6 modrm(mod=01) [sib] disp8
    i = blob.find(b"\x0f\xb6")
    while i != -1:
        modrm = blob[i + 2] if i + 2 < len(blob) else 0
        if (modrm >> 6) == 1 and (modrm & 7) != 4:
            disp = blob[i + 3]
            if disp == 0x30:
                window = blob[i + 4:i + 64]
                j = window.find(b"\x0f\xb6")
                while j != -1:
                    m2 = window[j + 2] if j + 2 < len(window) else 0
                    if (m2 >> 6) == 1 and (m2 & 7) != 4 and window[j + 3] == 0x31:
                        hits.append(img.base + rva + i)
                        break
                    j = window.find(b"\x0f\xb6", j + 1)
        i = blob.find(b"\x0f\xb6", i + 1)
print("%d candidate(s)" % len(hits))
for va in hits[:20]:
    off = img.va_to_off(va - 24)
    code = img.data[off:off + 140]
    print("--- 0x%X" % va)
    for insn in list(md.disasm(code, va - 24))[:28]:
        print("   0x%X: %-8s %s" % (insn.address, insn.mnemonic, insn.op_str))
