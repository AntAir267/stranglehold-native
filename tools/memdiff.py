"""Where two --dump-memory files differ (an A/B run's first differing frame, dumped both ways):
each differing run of bytes with its address and both sides' bytes.

    memdiff.py A.bin B.bin [--max N] [--skip LO-HI ...]

--skip leaves out an address range (hex), e.g. a thread's stack, which the memory hash leaves out
too. Each line: the first differing byte's address, how many differ, then both sides' bytes from
4 bytes before it.
"""
import struct, sys

def regions(path):
    d = open(path, 'rb').read()
    out, i = [], 0
    while i + 8 <= len(d):
        base, size = struct.unpack_from('<II', d, i)
        out.append((base, d[i + 8:i + 8 + size]))
        i += 8 + size
    return out

a, b = regions(sys.argv[1]), regions(sys.argv[2])
limit = int(sys.argv[sys.argv.index('--max') + 1]) if '--max' in sys.argv else 40
skips = [tuple(int(x, 16) for x in sys.argv[k + 1].split('-')) for k in range(len(sys.argv)) if sys.argv[k] == '--skip']
n = 0
for (base, da), (base2, db) in zip(a, b):
    if base != base2 or len(da) != len(db):
        print('regions differ at %08x' % base)
        continue
    if da == db:
        continue
    i = 0
    while i < len(da) and n < limit:
        if da[i] != db[i] and any(lo <= base + i < hi for lo, hi in skips):
            i += 1
            continue
        if da[i] != db[i]:
            j = i
            while j < len(da) and j - i < 64 and da[j] != db[j]:
                j += 1
            lo = max(0, i - 4)
            print('%08x (+%d): %s | %s' % (base + i, j - i, da[lo:j + 4].hex(), db[lo:j + 4].hex()))
            n += 1
            i = j + 4
        else:
            i += 1
print('%d differing runs shown' % n)
