"""Function boundaries and direct callers from eldenring.exe's exception table (.pdata), read-only.

usage: funcs.py <exe> range <hex va> [...]     the function (begin, end) containing each address
       funcs.py <exe> callers <hex va>          direct calls (E8 rel32) to a function start, with their functions
Unwind chains (functions split into several .pdata entries) are followed back to the primary entry.
"""
import bisect
import struct
import sys

sys.path.insert(0, __import__("os").path.dirname(__file__))
from xref import Image  # noqa: E402


def load_pdata(img):
    data = img.data
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    opt = pe + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    dirs = opt + (112 if magic == 0x20B else 96)
    rva, size = struct.unpack_from("<II", data, dirs + 3 * 8)  # IMAGE_DIRECTORY_ENTRY_EXCEPTION
    off = img.va_to_off(img.base + rva)
    entries = []
    for i in range(size // 12):
        begin, end, unwind = struct.unpack_from("<III", data, off + i * 12)
        entries.append((begin, end, unwind))
    entries.sort()
    return entries


def primary(img, entries, index):
    # Chained unwind info (flag UNW_FLAG_CHAININFO = 4) points back to the parent function's entry.
    begin, end, unwind = entries[index]
    seen = 0
    while seen < 16:
        info = img.va_to_off(img.base + (unwind & ~1))
        flags = img.data[info] >> 3
        if not flags & 4:
            return begin
        count = img.data[info + 2]
        chained = info + 4 + ((count + 1) & ~1) * 2
        begin, end, unwind = struct.unpack_from("<III", img.data, chained)
        seen += 1
    return begin


def containing(img, entries, va):
    rva = va - img.base
    i = bisect.bisect_right(entries, (rva, 0xFFFFFFFF, 0xFFFFFFFF)) - 1
    if i < 0 or not entries[i][0] <= rva < entries[i][1]:
        return None
    return img.base + primary(img, entries, i), img.base + entries[i][1]


def main():
    img = Image(sys.argv[1])
    entries = load_pdata(img)
    mode = sys.argv[2]
    if mode == "range":
        for a in sys.argv[3:]:
            r = containing(img, entries, int(a, 16))
            print("%s: %s" % (a, "not in a function" if r is None else "0x%X..0x%X" % r))
    elif mode == "callers":
        target = int(sys.argv[3], 16)
        for name, rva, vsize, raw, rsize, chars in img.code_sections():
            blob = img.data[raw:raw + rsize]
            base = img.base + rva
            i = blob.find(b"\xe8")
            while i != -1:
                if i + 5 <= len(blob) and base + i + 5 + struct.unpack_from("<i", blob, i + 1)[0] == target:
                    r = containing(img, entries, base + i)
                    print("call at 0x%X in %s" % (base + i, "?" if r is None else "0x%X" % r[0]))
                i = blob.find(b"\xe8", i + 1)


main()
