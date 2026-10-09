"""Find RIP-relative references to an address in eldenring.exe and disassemble around them (read-only).

usage: xref.py <exe> string <text>            references to a UTF-16 (then ASCII) string
       xref.py <exe> va <hex virtual address>  references to an address
       xref.py <exe> dis <hex va> [count]      disassemble from an address
Options: --before N (instructions shown before each reference, default 30), --after N (default 30)
"""
import struct
import sys

import capstone
import numpy as np


class Image:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        nt = struct.unpack_from("<I", self.data, 0x3C)[0]
        self.base = struct.unpack_from("<Q", self.data, nt + 24 + 24)[0]
        count = struct.unpack_from("<H", self.data, nt + 6)[0]
        opt_size = struct.unpack_from("<H", self.data, nt + 20)[0]
        self.sections = []
        for i in range(count):
            h = nt + 24 + opt_size + i * 40
            name = self.data[h:h + 8].rstrip(b"\0").decode()
            vsize, rva, rsize, raw = struct.unpack_from("<IIII", self.data, h + 8)
            chars = struct.unpack_from("<I", self.data, h + 36)[0]
            self.sections.append((name, rva, vsize, raw, rsize, chars))

    def off_to_va(self, off):
        for name, rva, vsize, raw, rsize, chars in self.sections:
            if raw <= off < raw + rsize:
                return self.base + rva + (off - raw)
        return None

    def va_to_off(self, va):
        rva = va - self.base
        for name, srva, vsize, raw, rsize, chars in self.sections:
            if srva <= rva < srva + rsize:
                return raw + (rva - srva)
        return None

    def code_sections(self):
        return [s for s in self.sections if s[5] & 0x20000000]


def find_refs(img, target):
    hits = []
    for name, rva, vsize, raw, rsize, chars in img.code_sections():
        blob = img.data[raw:raw + rsize]
        for align in range(4):
            n = (len(blob) - align) // 4
            disp = np.frombuffer(blob, dtype="<i4", count=n, offset=align).astype(np.int64)
            pos = align + 4 * np.arange(n, dtype=np.int64)
            dest = img.base + rva + pos + 4 + disp
            for p in np.nonzero(dest == target)[0]:
                hits.append(img.base + rva + int(pos[p]))  # VA of the disp32 field
    return sorted(hits)


def disasm(img, va, count, md):
    off = img.va_to_off(va)
    return list(md.disasm(img.data[off:off + count * 16], va))[:count]


def show_ref(img, disp_va, target, md, before, after):
    # Find the instruction that ends with this disp32 and uses RIP-relative addressing.
    for back in range(1, 16):
        insns = disasm(img, disp_va - back, 1, md)
        branch = insns and insns[0].mnemonic in ("call", "jmp") and insns[0].op_str == hex(target)
        if insns and insns[0].address + insns[0].size == disp_va + 4 and ("rip" in insns[0].op_str or branch):
            insn = insns[0]
            break
    else:
        print("  (no RIP-relative instruction ends at 0x%X)" % (disp_va + 4))
        return
    # Walk back roughly `before` instructions by re-disassembling from earlier starts that sync up.
    start = insn.address
    for back in range(before * 6, 0, -1):
        cand = disasm(img, insn.address - back, before + 4, md)
        if any(c.address == insn.address for c in cand):
            start = insn.address - back
            break
    for c in disasm(img, start, before + after + 1, md):
        mark = "=>" if c.address == insn.address else "  "
        print("%s 0x%X: %-8s %s" % (mark, c.address, c.mnemonic, c.op_str))


def main():
    path, mode, arg = sys.argv[1], sys.argv[2], sys.argv[3]
    before = int(sys.argv[sys.argv.index("--before") + 1]) if "--before" in sys.argv else 30
    after = int(sys.argv[sys.argv.index("--after") + 1]) if "--after" in sys.argv else 30
    img = Image(path)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    if mode == "dis":
        count = int(sys.argv[4]) if len(sys.argv) > 4 and not sys.argv[4].startswith("--") else 40
        for c in disasm(img, int(arg, 16), count, md):
            print("0x%X: %-8s %s" % (c.address, c.mnemonic, c.op_str))
        return
    if mode == "string":
        off = img.data.find(arg.encode("utf-16le") + b"\0\0")
        if off < 0:
            off = img.data.find(arg.encode() + b"\0")
        target = img.off_to_va(off)
        print("string at file 0x%X, VA 0x%X" % (off, target))
    else:
        target = int(arg, 16)
    refs = find_refs(img, target)
    print("%d reference(s) to 0x%X" % (len(refs), target))
    for r in refs[:12]:
        print("--- reference at 0x%X" % r)
        show_ref(img, r, target, md, before, after)


if __name__ == "__main__":
    main()
