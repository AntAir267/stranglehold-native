"""Unpack an Unreal Engine 3 Coalesced.ini / Coalesced.<lang> (PC little-endian or Xbox 360
big-endian lengths) into a folder of the .ini files it holds.

    coalesced.py <Coalesced file> <output folder>
"""
import os
import struct
import sys


def read(path):
    d = open(path, 'rb').read()
    for end in ('<', '>'):
        n = struct.unpack_from(end + 'I', d, 0)[0]
        if 0 < n < 10000:
            break
    out, p = [], 4
    for _ in range(n // 2):                 # (the count is of strings: a name and a text per file)
        ln = struct.unpack_from(end + 'i', d, p)[0]; p += 4
        if ln < 0:                       # UTF-16 name
            name = d[p:p - 2 * ln].decode('utf-16le' if end == '<' else 'utf-16be').rstrip('\0'); p += -2 * ln
        else:
            name = d[p:p + ln].decode('latin-1').rstrip('\0'); p += ln
        ln = struct.unpack_from(end + 'i', d, p)[0]; p += 4
        if ln < 0:
            text = d[p:p - 2 * ln].decode('utf-16le' if end == '<' else 'utf-16be').rstrip('\0'); p += -2 * ln
        else:
            text = d[p:p + ln].decode('latin-1').rstrip('\0'); p += ln
        out.append((name, text))
    return out


def main():
    src, dst = sys.argv[1:3]
    os.makedirs(dst, exist_ok=True)
    for name, text in read(src):
        base = name.replace('\\', '/').split('/')[-1]
        with open(os.path.join(dst, base), 'w', newline='') as fh:
            fh.write(text)
        print('%-50s %7d' % (base, len(text)))


if __name__ == '__main__':
    main()
