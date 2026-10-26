"""The game's own icon, from LARush.exe's resources, as a PNG for the applications menu.

    icon.py <LARush.exe> <out.png>

Reads the first icon group (RT_GROUP_ICON) and writes its largest image; needs Pillow to turn
the icon into a PNG (without it, nothing is written and the menu shows a generic icon).
"""
import io
import struct
import sys

sys.path.insert(0, __file__.rsplit('/', 1)[0])
from pe import PE

RT_ICON, RT_GROUP_ICON = 3, 14


def resources(pe):
    rva = pe.dirs[2][0]
    if not rva:
        return {}
    img = pe.image

    def entries(off):
        named, ids = struct.unpack_from('<HH', img, rva + off + 12)
        for i in range(named + ids):
            name, data = struct.unpack_from('<II', img, rva + off + 16 + 8 * i)
            yield name, data

    out = {}
    for typ, d1 in entries(0):
        for name, d2 in entries(d1 & 0x7fffffff):
            for lang, d3 in entries(d2 & 0x7fffffff):
                if d3 & 0x80000000:
                    continue
                data_rva, size = struct.unpack_from('<II', img, rva + d3)
                out.setdefault(typ, {})[name] = bytes(img[data_rva:data_rva + size])
    return out


def main():
    exe, out = sys.argv[1:3]
    res = resources(PE(exe))
    groups, icons = res.get(RT_GROUP_ICON, {}), res.get(RT_ICON, {})
    if not groups:
        sys.exit('no icon in %s' % exe)
    grp = groups[sorted(groups)[0]]
    count = struct.unpack_from('<H', grp, 4)[0]
    entries = [struct.unpack_from('<BBBBHHIH', grp, 6 + 14 * i) for i in range(count)]
    # an .ico file of the group: header, directory, then the images
    ico = struct.pack('<HHH', 0, 1, count)
    images, offset = b'', 6 + 16 * count
    for w, h, colors, _, planes, bits, size, ident in entries:
        data = icons[ident]
        ico += struct.pack('<BBBBHHII', w, h, colors, 0, planes, bits, len(data), offset + len(images))
        images += data
    try:
        from PIL import Image
    except ImportError:
        sys.exit('Pillow is needed to write the icon as a PNG')
    im = Image.open(io.BytesIO(ico + images))
    sizes = sorted(im.info.get('sizes', [im.size]))
    im.size = sizes[-1]
    im.load()
    im.convert('RGBA').save(out)
    print('%s: %dx%d' % (out, *im.size))


if __name__ == '__main__':
    main()
