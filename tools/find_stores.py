"""Find instructions that write to [reg + disp32] for a given displacement, decoded reliably (read-only).

usage: find_stores.py <exe> <hexdisp> [--xmm] [--context N]
For every occurrence of the displacement's bytes in the code sections, disassembles linearly from 96 bytes earlier
(x86 decoding resynchronizes within a few instructions) and keeps the instruction that actually contains those bytes
when it writes memory. Unlike find_disp.py this does not mistake SSE stores (0F 11 ..) for `adc`. --xmm keeps only
SSE stores; --context N prints N instructions before and after each hit.
"""
import struct
import sys

import capstone

sys.path.insert(0, __import__("os").path.dirname(__file__))
from xref import Image  # noqa: E402

img = Image(sys.argv[1])
disp = int(sys.argv[2], 16)
xmm_only = "--xmm" in sys.argv
context = int(sys.argv[sys.argv.index("--context") + 1]) if "--context" in sys.argv else 0
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
needle = "+ 0x%x]" % disp
pat = struct.pack("<I", disp)
for name, rva, vsize, raw, rsize, chars in img.code_sections():
    blob = img.data[raw:raw + rsize]
    i = blob.find(pat)
    while i != -1:
        start = max(0, i - 96)
        insns = list(md.disasm(blob[start:i + 16], img.base + rva + start))
        target = img.base + rva + i
        for k, insn in enumerate(insns):
            if insn.address <= target < insn.address + insn.size:
                ops = insn.op_str
                is_store = needle in ops.split(",")[0] and "ptr" in ops.split(",")[0] and \
                    not insn.mnemonic.startswith(("cmp", "test", "lea", "prefetch"))
                if is_store and (not xmm_only or "xmm" in ops):
                    print("0x%X: %s %s" % (insn.address, insn.mnemonic, ops))
                    if context:
                        more = list(md.disasm(blob[insn.address - img.base - rva - 64 * context // 4:
                                                   insn.address - img.base - rva + 8 * context],
                                              insn.address - 64 * context // 4))
                        idx = [j for j, c in enumerate(more) if c.address == insn.address]
                        if idx:
                            for c in more[max(0, idx[0] - context):idx[0] + context + 1]:
                                print("    %s0x%X: %-8s %s" % ("=> " if c.address == insn.address else "   ",
                                                              c.address, c.mnemonic, c.op_str))
                break
        i = blob.find(pat, i + 1)
