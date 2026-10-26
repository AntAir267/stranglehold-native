"""Several PE modules (the program and the DLLs translated with it) as one address space.

Each module sits at its own preferred base (the runtime maps it there; none overlap), so the
translator can treat them as one program: discovery follows calls between them, and a call or
jump through an import slot that another translated module satisfies becomes a direct call to
that module's function. Imports nothing translated provides are left to the runtime.
"""
import struct
from pe import PE
from x86 import decode, Insn

CODE = 0x20 | 0x20000000              # IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE


def exports(pe):
    """{name or '#ordinal': rva or forwarder string}"""
    out = {}
    rva, size = pe.dirs[0]
    if not rva:
        return out
    img = pe.image
    _, _, _, _, nm, ordbase, nfuncs, nnames, afuncs, anames, aords = struct.unpack_from('<IIHHIIIIIII', img, rva)
    funcs = [struct.unpack_from('<I', img, afuncs + 4 * k)[0] for k in range(nfuncs)]

    def val(f):
        if rva <= f < rva + size:                       # forwarder "DLL.name"
            return pe.cstr(pe.base + f)
        return f
    for k, f in enumerate(funcs):
        if f:
            out['#%d' % (ordbase + k)] = val(f)
    for k in range(nnames):
        n = pe.cstr(pe.base + struct.unpack_from('<I', img, anames + 4 * k)[0])
        o = struct.unpack_from('<H', img, aords + 2 * k)[0]
        out[n] = val(funcs[o])
    return out


def relocations(pe):
    """addresses of the dwords the base relocations patch (HIGHLOW entries)"""
    out = []
    rva, size = pe.dirs[5] if len(pe.dirs) > 5 else (0, 0)
    o = rva
    while o < rva + size:
        page, bsize = struct.unpack_from('<II', pe.image, o)
        if bsize < 8:
            break
        for k in range((bsize - 8) // 2):
            e = struct.unpack_from('<H', pe.image, o + 8 + 2 * k)[0]
            if e >> 12 == 3:
                out.append(pe.base + page + (e & 0xfff))
        o += bsize
    return out


class Module:
    def __init__(self, name, path):
        self.name = name                    # as imports name it, e.g. 'MFC71.dll'
        self.pe = PE(path)
        self.path = path
        p = self.pe
        self.lo, self.hi = p.base, p.base + p.size_of_image
        self.text = [(p.base + s['va'], p.base + s['va'] + max(s['vsize'], s['rsize']))
                     for s in p.sections if s['flags'] & CODE]
        self.exports = exports(p)
        self.is_dll = bool(p.characteristics & 0x2000)


class Image:
    """PE-like view of all modules: the methods discovery and the generator use"""

    def __init__(self, modules):
        self.modules = modules
        mods = sorted(modules, key=lambda m: m.lo)
        for a, b in zip(mods, mods[1:]):
            if a.hi > b.lo:
                raise ValueError('%s and %s overlap' % (a.name, b.name))
        self.by_name = {m.name.lower(): m for m in modules}
        self.main = modules[0]
        self.entry = self.main.pe.entry
        # every module's import slots: those another module provides are resolved here
        self.imports = []                   # (dll, name, iat va) the runtime provides
        self.resolved = {}                  # iat va -> function address in a translated module
        self.resolved_name = {}             # iat va -> 'DLL!name' (for comments and logs)
        for m in modules:
            for dll, name, va in m.pe.imports:
                t = self.resolve(dll, name)
                if isinstance(t, int):
                    self.resolved[va] = t
                    self.resolved_name[va] = '%s!%s' % (dll, name)
                else:
                    self.imports.append(t + (va,))

    def resolve(self, dll, name, depth=0):
        """function address, or (dll, name) left to the runtime (forwarders followed)"""
        m = self.by_name.get(dll.lower())
        if m is None or depth > 4:
            return (dll, name)
        v = m.exports.get(name)
        if v is None:
            raise ValueError('%s does not export %s' % (dll, name))
        if isinstance(v, str):
            fdll, fname = v.split('.', 1)
            if fname.startswith('#'):
                pass
            return self.resolve(fdll + '.dll', fname, depth + 1)
        return m.pe.base + v

    def module_at(self, va):
        for m in self.modules:
            if m.lo <= va < m.hi:
                return m
        return None

    def section_at(self, va):
        m = self.module_at(va)
        return m.pe.section_at(va) if m else None

    def u8(self, va):
        return self.module_at(va).pe.u8(va)

    def u32(self, va):
        return self.module_at(va).pe.u32(va)

    def bytes(self, va, n):
        return self.module_at(va).pe.bytes(va, n)

    def cstr(self, va):
        return self.module_at(va).pe.cstr(va)

    def seeds(self):
        """trusted entries: the program's entry, each DLL's entry and exported functions"""
        out = [self.entry]
        for m in self.modules[1:]:
            if m.pe.entry_rva:
                out.append(m.pe.entry)
            for v in m.exports.values():
                if isinstance(v, int) and self.in_text(m.pe.base + v):
                    out.append(m.pe.base + v)
        return out

    def in_text(self, a):
        for m in self.modules:
            if m.lo <= a < m.hi:
                return any(lo <= a < hi for lo, hi in m.text)
        return False

    def data_code_pointers(self):
        """aligned dwords in initialised data (and every relocated dword of the DLLs outside
        their code) that point into code; the relocated ones are also kept in self.reloc_pointers
        (a relocation marks a real address: text never has one)"""
        out = set()
        self.reloc_pointers = set()
        for m in self.modules:
            p = m.pe
            for s in p.sections:
                if s['flags'] & CODE or s['name'] in ('.rsrc', '.reloc'):
                    continue
                n = min(s['vsize'], s['rsize']) if s['vsize'] else s['rsize']
                for off in range(0, n - 3, 4):
                    v = struct.unpack_from('<I', p.image, s['va'] + off)[0]
                    if self.in_text(v):
                        out.add(v)
            for a in relocations(p):
                v = p.u32(a)
                if not self.in_text(v):
                    continue
                self.reloc_pointers.add(v)      # (sites in code too: immediates pushed as callbacks)
                if not any(lo <= a < hi for lo, hi in m.text):
                    out.add(v)
        return out


class Decoder:
    """Cached decoding across the modules; calls and jumps through a resolved import slot
    come out as direct ones to the providing function"""

    def __init__(self, img):
        self.img = img
        self.cache = {}

    def in_text(self, a):
        return self.img.in_text(a)

    def at(self, a):
        i = self.cache.get(a)
        if i is None and a not in self.cache:
            if not self.in_text(a):
                i = None
            else:
                i = decode(self.img.bytes(a, 16), a)
                if i is not None and i.mn in ('call', 'jmp') and i.ops[0][0] == 'm':
                    op = i.ops[0]
                    if op[3] is None and op[4] is None and op[2] in (None, 'ds') and op[6] in self.img.resolved:
                        t = self.img.resolved[op[6]]
                        i = Insn(i.addr, i.size, i.mn, [('i', t, 4)], i.raw,
                                 '%s %s' % (i.mn, self.img.resolved_name[op[6]]), i.prefixes)
            self.cache[a] = i
        return i
