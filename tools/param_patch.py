"""Find ELDEN RING's LoadBalancerParam in the running game's memory and show or patch it.

The param files stay in their on-disk PARAM layout in memory, with a 64-bit offset at +0x10 pointing to the
paramdef type name. This finds the type string "LOAD_BALANCER_PARAM_ST", walks back to the header that points at
it, and decodes the 80-byte rows (layout from libER's LOAD_BALANCER_PARAM_ST, based on Paramdex).

usage: param_patch.py show
       param_patch.py set <percent> [--level N]   # fixed dynamic resolution: min = max = percent
       param_patch.py restore                     # restore the values saved by the last `set`
Only for offline play with Easy Anti-Cheat off (me3).
"""
import ctypes
import ctypes.wintypes as wt
import json
import os
import struct
import sys

TYPE_NAME = b"LOAD_BALANCER_PARAM_ST\x00"
ROW_SIZE = 80
BACKUP = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "captures", "loadbalancer_backup.json")

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
PROCESS_ALL = 0x0010 | 0x0020 | 0x0008 | 0x0400  # VM_READ | VM_WRITE | VM_OPERATION | QUERY_INFORMATION
MEM_COMMIT, MEM_PRIVATE = 0x1000, 0x20000
READABLE = {0x02, 0x04, 0x08, 0x20, 0x40, 0x80}  # READONLY, READWRITE, WRITECOPY, EXECUTE_READ(+WRITE)(+COPY)


class MBI(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_void_p), ("AllocationBase", ctypes.c_void_p),
                ("AllocationProtect", wt.DWORD), ("PartitionId", wt.WORD), ("RegionSize", ctypes.c_size_t),
                ("State", wt.DWORD), ("Protect", wt.DWORD), ("Type", wt.DWORD)]


class PE32(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ProcessID", wt.DWORD),
                ("th32DefaultHeapID", ctypes.c_void_p), ("th32ModuleID", wt.DWORD), ("cntThreads", wt.DWORD),
                ("th32ParentProcessID", wt.DWORD), ("pcPriClassBase", ctypes.c_long), ("dwFlags", wt.DWORD),
                ("szExeFile", ctypes.c_char * 260)]


def find_pid(name=b"eldenring.exe"):
    snap = k32.CreateToolhelp32Snapshot(0x2, 0)
    entry = PE32()
    entry.dwSize = ctypes.sizeof(PE32)
    ok = k32.Process32First(snap, ctypes.byref(entry))
    try:
        while ok:
            if entry.szExeFile.lower() == name:
                return entry.th32ProcessID
            ok = k32.Process32Next(snap, ctypes.byref(entry))
    finally:
        k32.CloseHandle(snap)
    return None


def read(h, addr, size):
    buf = ctypes.create_string_buffer(size)
    got = ctypes.c_size_t()
    if not k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, size, ctypes.byref(got)):
        return None
    return buf.raw[:got.value]


def write(h, addr, data):
    got = ctypes.c_size_t()
    if not k32.WriteProcessMemory(h, ctypes.c_void_p(addr), data, len(data), ctypes.byref(got)):
        raise OSError(ctypes.get_last_error(), "WriteProcessMemory failed at 0x%X" % addr)


def regions(h):
    addr, mbi = 0, MBI()
    while k32.VirtualQueryEx(h, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)):
        base, size = mbi.BaseAddress or 0, mbi.RegionSize
        if mbi.State == MEM_COMMIT and mbi.Type == MEM_PRIVATE and (mbi.Protect & 0xFF) in READABLE \
                and not mbi.Protect & 0x100:
            yield base, size
        addr = base + size


def find_tables(h):
    """Yield (header_address, row_count) for each LoadBalancerParam file in memory."""
    for base, size in regions(h):
        for chunk_start in range(0, size, 64 << 20):
            chunk = read(h, base + chunk_start, min(64 << 20, size - chunk_start))
            if not chunk:
                continue
            pos = chunk.find(TYPE_NAME)
            while pos != -1:
                string_addr = base + chunk_start + pos
                # The header is 8-byte aligned, within 64 KiB before the type string. Chunks start on
                # page boundaries, so chunk offsets have the same alignment as addresses.
                window_start = max(0, pos - 0x10000) & ~7
                for hdr in range((pos - 0x40) & ~7, window_start - 1, -8):
                    if hdr < 0:
                        break
                    if struct.unpack_from("<Q", chunk, hdr + 0x10)[0] == pos - hdr:
                        rows = struct.unpack_from("<H", chunk, hdr + 0x0A)[0]
                        if 0 < rows < 1000:
                            yield base + chunk_start + hdr, rows
                        break
                pos = chunk.find(TYPE_NAME, pos + 1)


def rows_of(h, header, count):
    table = read(h, header + 0x40, count * 24)
    out = []
    for i in range(count):
        row_id, _, data_off, _ = struct.unpack_from("<IIQQ", table, i * 24)
        out.append((row_id, header + data_off))
    return out


def describe(data):
    lower, upper = struct.unpack_from("<ff", data, 0)
    return ("fps down<%.0f up>%.0f  dynamicResolution level %d  range %d%%..%d%%  AA level %d"
            % (lower, upper, data[42], data[48], data[49], data[27]))


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "show"
    pid = find_pid()
    if pid is None:
        sys.exit("eldenring.exe is not running")
    h = k32.OpenProcess(PROCESS_ALL, False, pid)
    if not h:
        sys.exit("OpenProcess failed: %d" % ctypes.get_last_error())
    tables = list(find_tables(h))
    if not tables:
        sys.exit("LoadBalancerParam not found")
    backup = {}
    for header, count in tables:
        print("LoadBalancerParam at 0x%X, %d rows" % (header, count))
        for row_id, addr in rows_of(h, header, count):
            data = read(h, addr, ROW_SIZE)
            print("  row %3d @0x%X: %s" % (row_id, addr, describe(data)))
            if cmd == "set":
                pct = int(sys.argv[2])
                level = int(sys.argv[sys.argv.index("--level") + 1]) if "--level" in sys.argv else 0
                backup["%X" % addr] = data[42:50].hex()
                write(h, addr + 42, bytes([level]))
                write(h, addr + 48, bytes([pct, pct]))
                print("        -> %s" % describe(read(h, addr, ROW_SIZE)))
    if cmd == "set":
        with open(BACKUP, "w") as f:
            json.dump(backup, f)
    elif cmd == "restore":
        with open(BACKUP) as f:
            saved = json.load(f)
        for addr, hexdata in saved.items():
            write(h, int(addr, 16) + 42, bytes.fromhex(hexdata))
        print("restored %d rows" % len(saved))
    k32.CloseHandle(h)


if __name__ == "__main__":
    main()
