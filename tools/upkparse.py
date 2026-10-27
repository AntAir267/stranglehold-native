"""Read a decompressed Stranglehold UE3 package (file version 375, licensee 25): summary, name,
import and export tables, and object paths. PC packages are little-endian, Xbox 360 big-endian;
the byte order is taken from the tag.

Summary layout as found in the data [verified 2026-10-26 on PC and 360 Startup_eng: all tables
parse to their recorded counts and the export table ends exactly at the depends offset]:
  tag, version (375 | 25<<16), header size, Midway 'WOO ' u32 (0x204F4F57), Woo version
  (PC 530, 360 523), a second Midway u32 (PC 35, 360 34), folder name FString, package flags,
  name count/offset, export count/offset, import count/offset, depends offset, guid,
  generations, engine version, cooker version, compression flags, compressed chunks.

    upkparse.py <file.upk> [filter]     list exports (path, class, size, offset)
"""
import struct
import sys


class Reader:
    def __init__(self, data, end, pos=0):
        self.d = data
        self.e = end
        self.p = pos

    def i32(self):
        v = struct.unpack_from(self.e + 'i', self.d, self.p)[0]
        self.p += 4
        return v

    def u32(self):
        v = struct.unpack_from(self.e + 'I', self.d, self.p)[0]
        self.p += 4
        return v

    def u64(self):
        v = struct.unpack_from(self.e + 'Q', self.d, self.p)[0]
        self.p += 8
        return v

    def f32(self):
        v = struct.unpack_from(self.e + 'f', self.d, self.p)[0]
        self.p += 4
        return v

    def u8(self):
        v = self.d[self.p]
        self.p += 1
        return v

    def fstring(self):
        n = self.i32()
        if n == 0:
            return ''
        if n < 0:
            raw = self.d[self.p:self.p - 2 * n]
            self.p += -2 * n
            return raw.decode('utf-16-be' if self.e == '>' else 'utf-16-le').rstrip('\0')
        raw = self.d[self.p:self.p + n]
        self.p += n
        return raw.rstrip(b'\0').decode('latin-1')


class Export:
    pass


class Import:
    pass


class Package:
    def __init__(self, data):
        self.d = data
        if data[:4] == b'\xc1\x83\x2a\x9e':
            self.e = '<'
        elif data[:4] == b'\x9e\x2a\x83\xc1':
            self.e = '>'
        else:
            raise ValueError('not a package')
        r = Reader(data, self.e, 4)
        self.version = r.u32()
        self.header_size = r.i32()
        self.woo_tag = r.u32()
        assert self.woo_tag == 0x204F4F57, hex(self.woo_tag)
        self.woo_version = r.u32()
        self.woo_extra = r.u32()
        self.folder = r.fstring()
        self.flags = r.u32()
        self.name_count, self.name_off = r.i32(), r.i32()
        self.export_count, self.export_off = r.i32(), r.i32()
        self.import_count, self.import_off = r.i32(), r.i32()
        self.depends_off = r.i32()
        r.p += 16                                   # guid
        gens = r.i32()
        self.generations = [(r.i32(), r.i32(), r.i32()) for _ in range(gens)]
        self.engine_version = r.i32()
        self.cooker_version = r.i32()
        self.compression = r.u32()
        self.chunks = r.i32()
        assert self.chunks == 0, 'compressed chunks inside package not handled'

        r.p = self.name_off
        self.names = []
        for _ in range(self.name_count):
            s = r.fstring()
            r.u64()
            self.names.append(s)

        r.p = self.import_off
        self.imports = []
        for _ in range(self.import_count):
            im = Import()
            im.class_package = self.fname(r)
            im.class_name = self.fname(r)
            im.outer = r.i32()
            im.name = self.fname(r)
            self.imports.append(im)

        r.p = self.export_off
        self.exports = []
        for _ in range(self.export_count):
            ex = Export()
            ex.class_index = r.i32()
            ex.super_index = r.i32()
            ex.outer = r.i32()
            ex.name = self.fname(r)
            ex.archetype = r.i32()
            ex.object_flags = r.u64()
            ex.size = r.i32()
            ex.offset = r.i32()
            ncomp = r.i32()
            ex.components = [(self.fname(r), r.i32()) for _ in range(ncomp)]
            ex.export_flags = r.u32()
            nnet = r.i32()
            ex.net_counts = [r.i32() for _ in range(nnet)]
            r.p += 16                               # package guid
            self.exports.append(ex)
        self.export_table_end = r.p

    def fname(self, r):
        i = r.i32()
        n = r.i32()
        if not (0 <= i < len(self.names)):
            return '<badname %d>' % i
        s = self.names[i]
        return s if n == 0 else '%s_%d' % (s, n - 1)

    def obj_name(self, idx):
        """Full path of an object reference (export > 0, import < 0, 0 = None)."""
        if idx == 0:
            return 'None'
        parts = []
        seen = 0
        while idx != 0 and seen < 64:
            seen += 1
            if idx > 0:
                if idx > len(self.exports):
                    return '<badexport %d>' % idx
                o = self.exports[idx - 1]
            else:
                if -idx > len(self.imports):
                    return '<badimport %d>' % idx
                o = self.imports[-idx - 1]
            parts.append(o.name)
            idx = o.outer
        return '.'.join(reversed(parts))

    def class_name(self, ex):
        if ex.class_index == 0:
            return 'Class'
        if ex.class_index < 0:
            return self.imports[-ex.class_index - 1].name
        return self.exports[ex.class_index - 1].name

    def export_data(self, ex):
        return self.d[ex.offset:ex.offset + ex.size]


def load(path):
    return Package(open(path, 'rb').read())


if __name__ == '__main__':
    pk = load(sys.argv[1])
    flt = sys.argv[2] if len(sys.argv) > 2 else ''
    print('version %#x woo %d/%d names %d exports %d imports %d table end %#x depends %#x' % (
        pk.version, pk.woo_version, pk.woo_extra, pk.name_count, pk.export_count, pk.import_count,
        pk.export_table_end, pk.depends_off))
    for i, ex in enumerate(pk.exports):
        path = pk.obj_name(i + 1)
        if flt in path:
            print('%5d %-70s %-28s %7d %#x' % (i + 1, path, pk.class_name(ex), ex.size, ex.offset))
