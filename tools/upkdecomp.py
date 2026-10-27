"""Decompress an Unreal Engine 3 fully compressed package (the CookedPC/CookedXenon .xxx files that
have a .uncompressed_size beside them): tag, block size, (compressed, uncompressed) totals, then
per block (compressed, uncompressed) sizes and the blocks. Blocks are LZO1X (through the
system's liblzo2) or zlib. The Xbox 360 files use the same container with a big-endian header,
and their blocks are LZO1X too, not LZX [verified 2026-10-26: all 48 fully compressed
CookedXenon packages, 1491 blocks, each decoded to exactly its listed size, totals match and
every result starts with the package tag; work/x360_lzo_check.txt]. The other 830 CookedXenon
.xxx files are plain packages (header tag then version 0x00190177) and are passed through.

    upkdecomp.py <file.xxx> <out.upk>
"""
import struct
import sys
import zlib
import ctypes

_lzo = None


def lzo1x(chunk, usize):
    global _lzo
    if _lzo is None:
        _lzo = ctypes.CDLL('liblzo2.so.2')
        _lzo.__lzo_init_v2(1, -1, -1, -1, -1, -1, -1, -1, -1, -1)
    out = ctypes.create_string_buffer(usize)
    n = ctypes.c_ulong(usize)
    r = _lzo.lzo1x_decompress_safe(chunk, ctypes.c_ulong(len(chunk)), out, ctypes.byref(n), None)
    if r != 0:
        raise ValueError('LZO error %d' % r)
    return out.raw[:n.value]


def decompress(d):
    for end in ('<', '>'):
        tag, block = struct.unpack_from(end + 'II', d, 0)
        if tag == 0x9e2a83c1:
            break
    else:
        raise ValueError('not a compressed package')
    if block == 0x00190177:          # plain package (file version 375, licensee 25), not compressed
        return bytes(d)
    csize, usize = struct.unpack_from(end + 'II', d, 8)
    n = (usize + block - 1) // block
    sizes = [struct.unpack_from(end + 'II', d, 16 + 8 * i) for i in range(n)]
    p = 16 + 8 * n
    out = bytearray()
    for cs, us in sizes:
        chunk = d[p:p + cs]
        p += cs
        if chunk[:1] == b'\x78':
            out += zlib.decompress(chunk)
        else:
            n0 = len(out)
            out += lzo1x(chunk, us)
            if len(out) - n0 != us:
                raise ValueError('block at %#x gave %d bytes, expected %d' % (p - cs, len(out) - n0, us))
    if len(out) != usize or out[:4] not in (b'\xc1\x83\x2a\x9e', b'\x9e\x2a\x83\xc1'):
        raise ValueError('bad total size or missing package tag')
    return bytes(out)


if __name__ == '__main__':
    d = open(sys.argv[1], 'rb').read()
    u = decompress(d)
    open(sys.argv[2], 'wb').write(u)
    print(len(d), '->', len(u))
