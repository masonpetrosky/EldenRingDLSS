"""Read, search and write the running game's memory from outside (stage 3 experiments).

Addresses are hex; a leading "s:" means a static address in eldenring.exe (e.g. s:1447F37A8), rebased to the
running module. "[a]+o" chains dereference: "[s:1447F37A8]+B0" is the graphics manager's field 0xB0.

usage: mem.py base                         # the running module's base
       mem.py read <address> [hex size]     # hex dump with floats
       mem.py scan <hex qword> [max]        # 8-byte aligned matches in private memory
       mem.py scanvt <static vtable>        # objects whose first qword is the rebased vtable
       mem.py write <address> <hex bytes>   # e.g. write s:143C28801 01
Only for offline play with Easy Anti-Cheat off (me3).
"""
import ctypes
import ctypes.wintypes as wt
import re
import struct
import sys

from param_patch import PROCESS_ALL, find_pid, k32, read, regions, write

STATIC_BASE = 0x140000000


class ME32(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("th32ModuleID", wt.DWORD), ("th32ProcessID", wt.DWORD),
                ("GlblcntUsage", wt.DWORD), ("ProccntUsage", wt.DWORD), ("modBaseAddr", ctypes.c_void_p),
                ("modBaseSize", wt.DWORD), ("hModule", ctypes.c_void_p), ("szModule", ctypes.c_char * 256),
                ("szExePath", ctypes.c_char * 260)]


def module_base(pid, name=b"eldenring.exe"):
    snap = k32.CreateToolhelp32Snapshot(0x8 | 0x10, pid)
    entry = ME32()
    entry.dwSize = ctypes.sizeof(ME32)
    ok = k32.Module32First(snap, ctypes.byref(entry))
    try:
        while ok:
            if entry.szModule.lower() == name:
                return entry.modBaseAddr
            ok = k32.Module32Next(snap, ctypes.byref(entry))
    finally:
        k32.CloseHandle(snap)
    return None


def resolve(h, base, text):
    """Evaluates an address expression: hex, s:<static>, [expr] (dereference) and +/- hex offsets."""
    text = text.strip()
    if text.startswith("["):
        depth = 0
        for i, c in enumerate(text):
            depth += c == "["
            depth -= c == "]"
            if depth == 0:
                break
        inner = read(h, resolve(h, base, text[1:i]), 8)
        if inner is None:
            raise ValueError("unreadable pointer in " + text)
        value, rest = struct.unpack("<Q", inner)[0], text[i + 1:]
    else:
        m = re.match(r"(s:)?([0-9a-fA-F]+)", text)
        value, rest = int(m.group(2), 16), text[m.end():]
        if m.group(1):
            value = value - STATIC_BASE + base
    for sign, number in re.findall(r"([+-])([0-9a-fA-F]+)", rest):
        value += int(number, 16) if sign == "+" else -int(number, 16)
    return value


def dump(address, data):
    for off in range(0, len(data), 16):
        row = data[off:off + 16]
        floats = " ".join("%10.4g" % struct.unpack_from("<f", row, i)[0] for i in range(0, len(row) - 3, 4))
        print("%016X  %-47s  %s" % (address + off, row.hex(" "), floats))


def scan(h, needle, limit):
    found = []
    for start, size in regions(h):
        for chunk_start in range(0, size, 64 << 20):
            chunk = read(h, start + chunk_start, min(64 << 20, size - chunk_start))
            if not chunk:
                continue
            pos = chunk.find(needle)
            while pos != -1:
                if pos % 8 == 0:
                    found.append(start + chunk_start + pos)
                    if len(found) >= limit:
                        return found
                pos = chunk.find(needle, pos + 1)
    return found


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    cmd, args = sys.argv[1], sys.argv[2:]
    pid = find_pid()
    if pid is None:
        sys.exit("eldenring.exe is not running")
    h = k32.OpenProcess(PROCESS_ALL, False, pid)
    if not h:
        sys.exit("OpenProcess failed: %d" % ctypes.get_last_error())
    base = module_base(pid)
    try:
        if cmd == "base":
            print("0x%X" % base)
        elif cmd == "read":
            address = resolve(h, base, args[0])
            size = int(args[1], 16) if len(args) > 1 else 0x40
            data = read(h, address, size)
            if data is None:
                sys.exit("unreadable: 0x%X" % address)
            dump(address, data)
        elif cmd in ("scan", "scanvt"):
            value = int(args[0], 16)
            if cmd == "scanvt":
                value = value - STATIC_BASE + base
            limit = int(args[1]) if len(args) > 1 else 50
            for address in scan(h, struct.pack("<Q", value), limit):
                print("0x%X" % address)
        elif cmd == "write":
            address = resolve(h, base, args[0])
            before = read(h, address, len(bytes.fromhex(args[1])))
            write(h, address, bytes.fromhex(args[1]))
            print("0x%X: %s -> %s" % (address, before.hex(" "), read(h, address, len(before)).hex(" ")))
        else:
            sys.exit(__doc__)
    finally:
        k32.CloseHandle(h)


if __name__ == "__main__":
    main()
