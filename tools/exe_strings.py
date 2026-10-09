"""Search eldenring.exe for ASCII/UTF-16 strings and RTTI names matching a regex (read-only)."""
import re
import sys

path, pattern = sys.argv[1], sys.argv[2]
data = open(path, "rb").read()
rx = re.compile(pattern.encode(), re.IGNORECASE)
seen = set()
# ASCII strings of printable characters
for m in re.finditer(rb"[\x20-\x7e]{6,}", data):
    s = m.group(0)
    if rx.search(s) and s not in seen:
        seen.add(s)
        print("A 0x%08X %s" % (m.start(), s.decode(errors="replace")[:160]))
# UTF-16LE strings
for m in re.finditer(rb"(?:[\x20-\x7e]\x00){6,}", data):
    s = m.group(0).decode("utf-16le", errors="replace").encode()
    if rx.search(s) and s not in seen:
        seen.add(s)
        print("W 0x%08X %s" % (m.start(), s.decode(errors="replace")[:160]))
