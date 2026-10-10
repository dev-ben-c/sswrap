#!/usr/bin/env python3
"""Find C++ classes and their vtables in Starsiege.exe (Borland C++, stripped).

The executable has no symbols, but Borland keeps a type descriptor for every polymorphic class,
with the class name inside it: a u16 at descriptor+6 holds the offset of the name from the
descriptor's start. Each vtable is preceded by a pointer to its class's descriptor (and two more
words), so: name -> descriptor -> vtable -> virtual functions.

    typeinfo.py Starsiege.exe 'SimGui::HudMtrRadar' 'TS::PerspectiveCamera'
    typeinfo.py Starsiege.exe --find-code <hex bytes from another build> [skip]

The second form looks for a function from one build in another: give its first bytes (hex,
'??' for bytes that differ, e.g. call targets); skip ignores that many leading bytes.
"""
import re, struct, sys


def load(path):
    b = open(path, 'rb').read()
    pe = struct.unpack_from('<I', b, 0x3c)[0]
    n = struct.unpack_from('<H', b, pe + 6)[0]
    opt = struct.unpack_from('<H', b, pe + 20)[0]
    base = struct.unpack_from('<I', b, pe + 52)[0]
    secs = []
    for i in range(n):
        name, vsize, va, rsize, raw = struct.unpack_from('<8sIIII', b, pe + 24 + opt + 40 * i)
        secs.append((name.rstrip(b'\0').decode(), base + va, max(vsize, rsize), raw))
    return b, secs


def off2va(secs, o):
    for _, va, size, raw in secs:
        if raw <= o < raw + size:
            return va + o - raw


def section_of(secs, va):
    for name, start, size, _ in secs:
        if start <= va < start + size:
            return name


def descriptor(b, secs, cls):
    m = b.find(cls.encode() + b'\0')
    while m > 0 and (b[m - 1:m].isalnum() or b[m - 1:m] == b':'):
        m = b.find(cls.encode() + b'\0', m + 1)
    if m < 0:
        return None
    for back in range(8, 257):
        if struct.unpack_from('<H', b, m - back + 6)[0] == back:
            return off2va(secs, m - back)


def vtables(b, secs, cls):
    d = descriptor(b, secs, cls)
    if d is None:
        return d, []
    text = [s for s in secs if s[0] == '.text'][0]
    out = []
    for q in re.finditer(re.escape(struct.pack('<I', d)), b):
        p = off2va(secs, q.start())
        if section_of(secs, p) == '.text':
            continue
        slots, k = [], 0
        while True:
            v = struct.unpack_from('<I', b, q.start() + 12 + 4 * k)[0]
            if not text[1] <= v < text[1] + text[2]:
                break
            slots.append(v); k += 1
        if len(slots) >= 4:
            out.append((p + 12, slots))
    return d, out


if __name__ == '__main__':
    b, secs = load(sys.argv[1])
    if sys.argv[2] == '--find-code':
        tokens = sys.argv[3].split()
        skip = int(sys.argv[4]) if len(sys.argv) > 4 else 0
        rx = b''.join(b'.' if t == '??' else re.escape(bytes([int(t, 16)])) for t in tokens[skip:])
        for m in re.finditer(rx, b, re.S):
            print(hex(off2va(secs, m.start() - skip)))
        sys.exit()
    for cls in sys.argv[2:]:
        d, vts = vtables(b, secs, cls)
        print(f'{cls}: descriptor {d:#x}' if d else f'{cls}: not found')
        for vt, slots in vts:
            print(f'  vtable {vt:#x}: ' + ' '.join(f'{i}:{s:x}' for i, s in enumerate(slots)))
