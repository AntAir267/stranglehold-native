#!/usr/bin/env python3
"""Direct3D 9 shader bytecode: disassembler and assembler (vs_1_1-vs_3_0, ps_1_1-ps_3_0 tokens; the
game uses vs_2_0, ps_1_1 and ps_2_0), and the shader substitution's build step.

    d3dasm.py dis <file> [index ...]   bytecode as text: a .pak (u32 size + bytecode, repeated:
                                       the game's vshaders.pak / pshaders.pak) or one raw shader
    d3dasm.py asm <in.vsh|.psh> [-o out.bin]   text to bytecode (prints the hash)
    d3dasm.py hash <file>              the hash of a raw shader (or of each in a .pak)
    d3dasm.py dump [game dir] [out]    every shader of the paks as text named by its hash:
                                       <out>/<hash>.vsh / .psh (default ~/.local/share/
                                       stranglehold-native/game, work/shaders); index.txt lists them
    d3dasm.py selftest [game dir]      every shader of the paks disassembled and assembled again:
                                       byte-identical, and no instruction left as raw tokens
    d3dasm.py ctable <dir> <out.c>     (the Makefile) the replacements in <dir> assembled into a C
                                       table for runtime/d3d9.c; the file rewritten only if changed

REPLACING A SHADER (runtime/d3d9.c, option shaders = original | improved): `dump`, copy the file
of the shader to change from work/shaders into data/shaders keeping its name (the FNV-1a 64 hash
of the original bytecode, the game's shader it replaces), edit it, `make`. With shaders = improved
the game's CreateVertexShader / CreatePixelShader of that bytecode gets ours instead (logged once).
Keep the constant registers and the vertex inputs the game feeds; the dumped file's header names
the pak and index. Fog stays the fixed-function table fog DXVK applies for shader models 1-2: a
ps_3_0 replacement would have to fog itself.

THE TEXT is Microsoft's shader assembly as D3DXDisassembleShader writes it, with these choices:
- one instruction a line; ';' or '//' start a comment; the end token is implied
- write masks and swizzles as .xyzw (.rgba accepted); a swizzle is written shortest: its last
  letter repeats (.xyz = .xyzz, .w = .wwww)
- source modifiers: -r0, 1 - r0, r0_bias, r0_bx2, r0_x2, r0_dz, r0_dw, r0_abs, !b0 (with '-' where
  the token says so: -r0_bx2); a modifier is printed before the swizzle (t0_bx2.w; both orders read)
- instruction modifiers from the destination token: _sat _pp _centroid _x2 _x4 _x8 _d2 _d4 _d8;
  ps_1_x co-issue as a leading '+'; ifc/breakc/setp comparisons as _gt _eq _ge _lt _ne _le
- relative addressing c5[a0.x]; texldp / texldb for texld's project / bias controls
- def c0, 1, 0.5, 0, -2 with each float printed shortest that reads back to the same bits
  (non-finite values as 0x7fc00000); defi i0, 1, 2, 3, 4; defb b0, true
- dcl_position v0, dcl_texcoord1 v2 (usage index 0 unnumbered), dcl_2d s0, dcl t0.xy (ps_2_0's
  input dcl, whose usage token is bare)
- a comment token (the compiler's constant table, say) as `.comment <hex dwords>` (a CTAB is
  decoded into ';' lines above it); anything the text could not reproduce exactly as `.dword
  <hex tokens>` (selftest requires none)
"""
import os
import re
import struct
import sys

# opcode: (name, has destination, sources). Sources are only used to tell a missing operand; the
# token count comes from the instruction's length (shader model 2+) or the parameter tokens'
# bit 31 (shader model 1).
OPS = {
    0: ('nop', 0, 0), 1: ('mov', 1, 1), 2: ('add', 1, 2), 3: ('sub', 1, 2), 4: ('mad', 1, 3),
    5: ('mul', 1, 2), 6: ('rcp', 1, 1), 7: ('rsq', 1, 1), 8: ('dp3', 1, 2), 9: ('dp4', 1, 2),
    10: ('min', 1, 2), 11: ('max', 1, 2), 12: ('slt', 1, 2), 13: ('sge', 1, 2), 14: ('exp', 1, 1),
    15: ('log', 1, 1), 16: ('lit', 1, 1), 17: ('dst', 1, 2), 18: ('lrp', 1, 3), 19: ('frc', 1, 1),
    20: ('m4x4', 1, 2), 21: ('m4x3', 1, 2), 22: ('m3x4', 1, 2), 23: ('m3x3', 1, 2), 24: ('m3x2', 1, 2),
    25: ('call', 0, 1), 26: ('callnz', 0, 2), 27: ('loop', 0, 2), 28: ('ret', 0, 0),
    29: ('endloop', 0, 0), 30: ('label', 0, 1), 32: ('pow', 1, 2), 33: ('crs', 1, 2),
    34: ('sgn', 1, 3), 35: ('abs', 1, 1), 36: ('nrm', 1, 1), 37: ('sincos', 1, 3), 38: ('rep', 0, 1),
    39: ('endrep', 0, 0), 40: ('if', 0, 1), 41: ('ifc', 0, 2), 42: ('else', 0, 0), 43: ('endif', 0, 0),
    44: ('break', 0, 0), 45: ('breakc', 0, 2), 46: ('mova', 1, 1), 64: ('texcoord', 1, 0),
    65: ('texkill', 1, 0), 66: ('tex', 1, 0), 67: ('texbem', 1, 1), 68: ('texbeml', 1, 1),
    69: ('texreg2ar', 1, 1), 70: ('texreg2gb', 1, 1), 71: ('texm3x2pad', 1, 1), 72: ('texm3x2tex', 1, 1),
    73: ('texm3x3pad', 1, 1), 74: ('texm3x3tex', 1, 1), 76: ('texm3x3spec', 1, 2),
    77: ('texm3x3vspec', 1, 1), 78: ('expp', 1, 1), 79: ('logp', 1, 1), 80: ('cnd', 1, 3),
    82: ('texreg2rgb', 1, 1), 83: ('texdp3tex', 1, 1), 84: ('texm3x2depth', 1, 1), 85: ('texdp3', 1, 1),
    86: ('texm3x3', 1, 1), 87: ('texdepth', 1, 0), 88: ('cmp', 1, 3), 89: ('bem', 1, 2),
    90: ('dp2add', 1, 3), 91: ('dsx', 1, 1), 92: ('dsy', 1, 1), 93: ('texldd', 1, 4), 94: ('setp', 1, 2),
    95: ('texldl', 1, 2), 96: ('breakp', 0, 1), 0xfffd: ('phase', 0, 0),
}
OP_DCL, OP_DEFB, OP_DEFI, OP_DEF, OP_TEX, OP_TEXCOORD = 31, 47, 48, 81, 66, 64
OP_COMMENT, OP_END = 0xfffe, 0xffff
COMPARISONS = {1: 'gt', 2: 'eq', 3: 'ge', 4: 'lt', 5: 'ne', 6: 'le'}
CMP_OPS = (41, 45, 94)
USAGES = ['position', 'blendweight', 'blendindices', 'normal', 'psize', 'texcoord', 'tangent',
          'binormal', 'tessfactor', 'positiont', 'color', 'fog', 'depth', 'sample']
TEXTYPES = {2: '2d', 3: 'cube', 4: 'volume'}
SRCMOD_SUFFIX = {2: '_bias', 3: '_bias', 4: '_bx2', 5: '_bx2', 7: '_x2', 8: '_x2', 9: '_dz', 10: '_dw',
                 11: '_abs', 12: '_abs'}
SRCMOD_NEG = {1, 3, 5, 8, 12}
SHIFTS = {1: '_x2', 2: '_x4', 3: '_x8', 15: '_d2', 14: '_d4', 13: '_d8'}
RESMODS = {1: '_sat', 2: '_pp', 4: '_centroid'}
XYZW = 'xyzw'


class AsmError(Exception):
    pass


def opname(op, ps, major, minor, ctrl):
    if op == OP_TEX:
        if ps and major == 1 and minor < 4:
            return 'tex'
        return {0: 'texld', 1: 'texldp', 2: 'texldb'}.get(ctrl, 'texld')
    if op == OP_TEXCOORD and ps and major == 1 and minor == 4:
        return 'texcrd'
    return OPS[op][0]


# ------------------------------------------------------------------ registers
def regname(typ, n, ps, major):
    if typ == 0: return 'r%d' % n
    if typ == 1: return 'v%d' % n
    if typ == 2: return 'c%d' % n
    if typ == 3: return ('t%d' if ps else 'a%d') % n
    if typ == 4: return ['oPos', 'oFog', 'oPts'][n] if n < 3 else None
    if typ == 5: return 'oD%d' % n
    if typ == 6: return ('o%d' if major >= 3 else 'oT%d') % n
    if typ == 7: return 'i%d' % n
    if typ == 8: return 'oC%d' % n
    if typ == 9: return 'oDepth' if n == 0 else None
    if typ == 10: return 's%d' % n
    if typ == 11: return 'c%d' % (n + 2048)
    if typ == 12: return 'c%d' % (n + 4096)
    if typ == 13: return 'c%d' % (n + 6144)
    if typ == 14: return 'b%d' % n
    if typ == 15: return 'aL' if n == 0 else None
    if typ == 17: return ['vPos', 'vFace'][n] if n < 2 else None
    if typ == 18: return 'l%d' % n
    if typ == 19: return 'p%d' % n
    return None


REG_RE = re.compile(r'(oPos|oFog|oPts|oDepth|vPos|vFace|aL|oD|oT|oC|[rvctaoisblp])(\d*)$')


def parse_reg(s, ps, major):
    m = REG_RE.match(s)
    if not m:
        raise AsmError('not a register: %r' % s)
    p, d = m.group(1), m.group(2)
    n = int(d) if d else 0
    fixed = {'oPos': (4, 0), 'oFog': (4, 1), 'oPts': (4, 2), 'oDepth': (9, 0), 'vPos': (17, 0),
             'vFace': (17, 1), 'aL': (15, 0)}
    if p in fixed:
        if d:
            raise AsmError('register %r takes no number' % s)
        return fixed[p]
    if not d:
        raise AsmError('register %r needs a number' % s)
    if p == 'c':
        for base, typ in ((6144, 13), (4096, 12), (2048, 11)):
            if n >= base:
                return typ, n - base
        return 2, n
    t = {'r': 0, 'v': 1, 'a': 3, 't': 3, 'oD': 5, 'oT': 6, 'o': 6, 'i': 7, 'oC': 8, 's': 10, 'b': 14,
         'l': 18, 'p': 19}[p]
    if (p == 't' and not ps) or (p == 'a' and ps):
        raise AsmError('register %r is not in a %s shader' % (s, 'pixel' if ps else 'vertex'))
    return t, n


def reg_bits(typ, n):
    return (n & 0x7ff) | ((typ & 7) << 28) | ((typ & 0x18) << 8)


def tok_reg(t):
    return ((t >> 28) & 7) | ((t >> 8) & 0x18), t & 0x7ff


# ------------------------------------------------------------------ masks and swizzles
def mask_text(m):
    return '' if m == 15 else '.' + ''.join(XYZW[i] for i in range(4) if m >> i & 1)


def swz_text(s):
    if s == 0xe4:
        return ''
    c = [(s >> (2 * i)) & 3 for i in range(4)]
    k = 4
    while k > 1 and c[k - 1] == c[k - 2]:
        k -= 1
    return '.' + ''.join(XYZW[x] for x in c[:k])


def comp_index(ch):
    i = 'xyzwrgba'.find(ch)
    if i < 0:
        raise AsmError('bad component %r' % ch)
    return i & 3


def parse_mask(s):
    m = 0
    last = -1
    for ch in s:
        i = comp_index(ch)
        if i <= last:
            raise AsmError('write mask .%s out of order' % s)
        last = i
        m |= 1 << i
    return m


def parse_swz(s):
    if not 1 <= len(s) <= 4:
        raise AsmError('bad swizzle .%s' % s)
    c = [comp_index(ch) for ch in s]
    c += [c[-1]] * (4 - len(c))
    return c[0] | c[1] << 2 | c[2] << 4 | c[3] << 6


# ------------------------------------------------------------------ floats
def float_text(bits):
    f = struct.unpack('<f', struct.pack('<I', bits))[0]
    if f != f or f in (float('inf'), float('-inf')):
        return '0x%08x' % bits
    for p in range(1, 10):
        s = '%.*g' % (p, f)
        if struct.unpack('<I', struct.pack('<f', float(s)))[0] == bits:
            return s
    return '0x%08x' % bits


def float_bits(s):
    if s.lower().startswith('0x') or s.lower().startswith('-0x'):
        return int(s, 16) & 0xffffffff
    try:
        return struct.unpack('<I', struct.pack('<f', float(s)))[0]
    except (ValueError, OverflowError):
        raise AsmError('bad float %r' % s)


# ------------------------------------------------------------------ disassembly
class Dis:
    def __init__(self, code):
        if len(code) % 4:
            raise AsmError('bytecode length %d is not a multiple of 4' % len(code))
        self.w = list(struct.unpack('<%dI' % (len(code) // 4), code))
        v = self.w[0]
        if v >> 16 not in (0xfffe, 0xffff):
            raise AsmError('not a shader version token: %08x' % v)
        self.ps = v >> 16 == 0xffff
        self.major, self.minor = (v >> 8) & 0xff, v & 0xff

    def dst(self, t):
        typ, n = tok_reg(t)
        name = regname(typ, n, self.ps, self.major)
        if name is None:
            raise AsmError('register type %d number %d' % (typ, n))
        if t & (1 << 13):
            raise AsmError('relative destination')
        return name + mask_text((t >> 16) & 15)

    def mods(self, t):
        s = ''
        rm = (t >> 20) & 15
        for b in (1, 2, 4):
            if rm & b:
                s += RESMODS[b]
        if rm & 8:
            raise AsmError('result modifier %x' % rm)
        sh = (t >> 24) & 15
        if sh:
            s = SHIFTS.get(sh, '?') + s
        return s

    def src(self, toks, i):
        """a source at toks[i]: (text, tokens used)"""
        t = toks[i]
        typ, n = tok_reg(t)
        name = regname(typ, n, self.ps, self.major)
        if name is None:
            raise AsmError('register type %d number %d' % (typ, n))
        used = 1
        if t & (1 << 13):
            if self.major >= 2:
                r = toks[i + 1]
                used = 2
                rtyp, rn = tok_reg(r)
                rname = regname(rtyp, rn, self.ps, self.major)
                if rname is None:
                    raise AsmError('relative register')
                if rtyp == 15:
                    name += '[aL]'
                else:
                    name += '[%s.%s]' % (rname, XYZW[(r >> 16) & 3])
            else:
                name += '[a0.x]'
        mod = (t >> 24) & 15
        s = name + SRCMOD_SUFFIX.get(mod, '') + swz_text((t >> 16) & 0xff)
        if mod == 6:
            s = '1 - ' + s
        elif mod == 13:
            s = '!' + s
        elif mod in SRCMOD_NEG:
            s = '-' + s
        elif mod not in (0, 2, 4, 7, 9, 10, 11):
            raise AsmError('source modifier %d' % mod)
        return s, used

    def instr(self, toks):
        """text of one instruction (toks: its tokens, the instruction token first)"""
        t = toks[0]
        op = t & 0xffff
        ctrl = (t >> 16) & 0xff
        if t & 0x80000000 or t & (1 << 29) or t & (1 << 28):
            raise AsmError('instruction token bits %08x' % t)
        pre = '+' if t & (1 << 30) else ''
        if pre and not (self.ps and self.major == 1):
            raise AsmError('co-issue outside ps_1_x')
        p = toks[1:]
        if op == OP_DCL:
            u, d = p
            typ, n = tok_reg(d)
            if typ == 10:
                tt = (u >> 27) & 15
                if u != (0x80000000 | tt << 27) or tt not in TEXTYPES:
                    raise AsmError('sampler dcl %08x' % u)
                head = 'dcl_' + TEXTYPES[tt]
            elif self.ps and self.major < 3 and u == 0x80000000:
                head = 'dcl'
            else:
                us, ui = u & 31, (u >> 16) & 15
                if u != (0x80000000 | us | ui << 16) or us >= len(USAGES):
                    raise AsmError('dcl usage %08x' % u)
                head = 'dcl_' + USAGES[us] + (str(ui) if ui else '')
            return pre + head + self.mods(d) + ' ' + self.dst(d)
        if op == OP_DEF:
            return pre + 'def %s, %s' % (self.dst(p[0]), ', '.join(float_text(x) for x in p[1:5]))
        if op == OP_DEFI:
            return pre + 'defi %s, %s' % (self.dst(p[0]), ', '.join(str(struct.unpack('<i', struct.pack('<I', x))[0]) for x in p[1:5]))
        if op == OP_DEFB:
            if p[1] not in (0, 1):
                raise AsmError('defb value %x' % p[1])
            return pre + 'defb %s, %s' % (self.dst(p[0]), 'true' if p[1] else 'false')
        if op not in OPS:
            raise AsmError('opcode %d' % op)
        name = opname(op, self.ps, self.major, self.minor, ctrl)
        if op in CMP_OPS:
            if ctrl not in COMPARISONS:
                raise AsmError('comparison %d' % ctrl)
            name += '_' + COMPARISONS[ctrl]
        elif ctrl and not (op == OP_TEX and name != 'tex' and ctrl in (1, 2)):
            raise AsmError('controls %02x on %s' % (ctrl, name))
        args = []
        i = 0
        if OPS[op][1] and p:
            name += self.mods(p[0])
            args.append(self.dst(p[0]))
            i = 1
        while i < len(p):
            s, used = self.src(p, i)
            args.append(s)
            i += used
        return pre + name + (' ' + ', '.join(args) if args else '')

    def split(self):
        """the program as (instruction tokens) lists, comments and all, ending before the end token"""
        w = self.w
        i = 1
        out = []
        while i < len(w):
            t = w[i]
            op = t & 0xffff
            if t == OP_END:
                if i != len(w) - 1:
                    raise AsmError('tokens after the end token')
                return out
            if op == OP_COMMENT:
                n = (t >> 16) & 0x7fff
            elif self.major >= 2:
                n = (t >> 24) & 15
            elif op == OP_DEF:
                n = 5
            else:
                n = 0
                while i + 1 + n < len(w) and w[i + 1 + n] & 0x80000000:
                    n += 1
            out.append(w[i:i + 1 + n])
            i += 1 + n
        raise AsmError('no end token')

    def text(self, strict=False):
        lines = ['%s_%d_%d' % ('ps' if self.ps else 'vs', self.major, self.minor)]
        nraw = 0
        for toks in self.split():
            if toks[0] & 0xffff == OP_COMMENT and not toks[0] & 0x80000000:
                lines += ctab_lines(toks[1:])
                lines.append('.comment' + ''.join(' %08x' % x for x in toks[1:]))
                continue
            try:
                s = self.instr(toks)
                if assemble_line(s, self.ps, self.major, self.minor) != toks:
                    raise AsmError('does not assemble back the same')
            except (AsmError, ValueError, IndexError, KeyError) as e:
                if strict:
                    raise AsmError('%s: %s' % (' '.join('%08x' % x for x in toks), e))
                nraw += 1
                s = '.dword%s    ; %s' % (''.join(' %08x' % x for x in toks), e)
            lines.append(s)
        self.nraw = nraw
        return lines


def ctab_lines(c):
    """a constant table comment (D3DXSHADER_CONSTANTTABLE) as readable ';' lines"""
    if not c or c[0] != 0x42415443:          # 'CTAB'
        return []
    b = struct.pack('<%dI' % len(c), *c)[4:]
    try:
        def cstr(o):
            e = b.index(b'\0', o)
            return b[o:e].decode('latin-1')
        size, creator, ver, n, info = struct.unpack_from('<5I', b, 0)
        out = ['; constant table (%s)' % cstr(creator)]
        for k in range(n):
            nm, rset, ridx, rcnt = struct.unpack_from('<IHHH', b, info + k * 20)
            out.append(';   %-24s %s%d%s' % (cstr(nm), 'bifs'[rset] if rset < 4 else '?', ridx,
                                            ' (%d)' % rcnt if rcnt > 1 else ''))
        return out
    except (ValueError, struct.error, IndexError):
        return []


def disassemble(code, strict=False):
    d = Dis(code)
    lines = d.text(strict)
    return lines, d.nraw


# ------------------------------------------------------------------ assembly
def split_operands(s):
    return [a.strip() for a in s.split(',')] if s.strip() else []


SRC_RE = re.compile(r'^(?P<pre>-|!|1\s*-\s*)?\s*(?P<reg>[A-Za-z]+\d*)(?:\[(?P<rel>[^\]]*)\])?'
                    r'(?P<rest>(?:_[a-z0-9]+|\.[xyzwrgba]+)*)$')


def enc_src(s, ps, major):
    m = SRC_RE.match(s.replace(' ', '') if not s.startswith('1') else s)
    if not m:
        raise AsmError('bad source %r' % s)
    pre = (m.group('pre') or '').replace(' ', '')
    typ, n = parse_reg(m.group('reg'), ps, major)
    t = 0x80000000 | reg_bits(typ, n)
    extra = []
    if m.group('rel') is not None:
        r = m.group('rel').strip()
        t |= 1 << 13
        if major >= 2:
            if r == 'aL':
                extra.append(0x80000000 | reg_bits(15, 0) | 0xe4 << 16)
            else:
                mm = re.match(r'^([a-z]+\d*)\.([xyzw])$', r)
                if not mm:
                    raise AsmError('bad relative address %r' % r)
                rt, rn = parse_reg(mm.group(1), ps, major)
                c = comp_index(mm.group(2))
                extra.append(0x80000000 | reg_bits(rt, rn) | (c * 0x55) << 16)
        elif r != 'a0.x':
            raise AsmError('shader model 1 addresses through a0.x only')
    swz = 0xe4
    suffix = None
    for part in re.findall(r'_[a-z0-9]+|\.[xyzwrgba]+', m.group('rest')):
        if part[0] == '.':
            swz = parse_swz(part[1:])
        else:
            if suffix:
                raise AsmError('two source modifiers in %r' % s)
            suffix = part
    if pre == '1-':
        if suffix:
            raise AsmError('1 - with a modifier')
        mod = 6
    elif pre == '!':
        if suffix:
            raise AsmError('! with a modifier')
        mod = 13
    else:
        neg = pre == '-'
        base = {None: 0, '_bias': 2, '_bx2': 4, '_x2': 7, '_dz': 9, '_dw': 10, '_abs': 11}
        if suffix not in base:
            raise AsmError('unknown source modifier %r' % suffix)
        mod = base[suffix]
        if neg:
            mod = {0: 1, 2: 3, 4: 5, 7: 8, 11: 12}.get(mod)
            if mod is None:
                raise AsmError('%s cannot be negated' % suffix)
    return [t | swz << 16 | mod << 24] + extra


def enc_dst(s, ps, major, mods):
    m = re.match(r'^([A-Za-z]+\d*)(?:\.([xyzwrgba]+))?$', s)
    if not m:
        raise AsmError('bad destination %r' % s)
    typ, n = parse_reg(m.group(1), ps, major)
    mask = parse_mask(m.group(2)) if m.group(2) else 15
    return 0x80000000 | reg_bits(typ, n) | mask << 16 | mods


def take_mods(name):
    """'mul_x2_sat' -> ('mul', destination modifier bits, comparison)"""
    parts = name.split('_')
    base = parts[0]
    bits = 0
    cmp = 0
    rev = {v: k for k, v in SHIFTS.items()}
    for p in parts[1:]:
        p = '_' + p
        if p in rev:
            if bits & 0x0f000000:
                raise AsmError('two shifts in %r' % name)
            bits |= rev[p] << 24
        elif p == '_sat':
            bits |= 1 << 20
        elif p == '_pp':
            bits |= 2 << 20
        elif p == '_centroid':
            bits |= 4 << 20
        elif p[1:] in COMPARISONS.values() and not cmp:
            cmp = [k for k, v in COMPARISONS.items() if v == p[1:]][0]
        else:
            raise AsmError('unknown modifier %s in %r' % (p, name))
    return base, bits, cmp


NAMES = {}
for _op, (_n, _d, _s) in OPS.items():
    NAMES[_n] = _op
NAMES.update({'texld': OP_TEX, 'texldp': OP_TEX, 'texldb': OP_TEX, 'texcrd': OP_TEXCOORD})


def assemble_line(line, ps, major, minor):
    """one instruction's tokens"""
    s = line.strip()
    co = 0
    if s.startswith('+'):
        co = 1 << 30
        s = s[1:].strip()
    m = re.match(r'^(\S+)\s*(.*)$', s)
    head, rest = m.group(1), m.group(2)
    ops = split_operands(rest)
    if head == '.dword':
        return [int(x, 16) for x in rest.split()]
    if head == '.comment':
        d = [int(x, 16) for x in rest.split()]
        return [OP_COMMENT | len(d) << 16] + d
    if head.startswith('dcl'):
        h = head.split('_')
        # dcl[_usage[N] | _2d ...][_pp _centroid ...]
        mods_from = 1
        if len(h) > 1 and h[1] in TEXTYPES.values():
            u = 0x80000000 | [k for k, v in TEXTYPES.items() if v == h[1]][0] << 27
            mods_from = 2
        elif len(h) > 1 and re.match(r'^[a-z]+\d*$', h[1]) and re.match(r'^[a-z]+', h[1]).group(0) in USAGES:
            us = re.match(r'^([a-z]+)(\d*)$', h[1])
            u = 0x80000000 | USAGES.index(us.group(1)) | (int(us.group(2) or 0) << 16)
            mods_from = 2
        else:
            u = 0x80000000
        _, bits, _ = take_mods('dcl' + ''.join('_' + x for x in h[mods_from:]))
        if len(ops) != 1:
            raise AsmError('dcl takes one register')
        return [co | OP_DCL | (2 << 24 if major >= 2 else 0), u, enc_dst(ops[0], ps, major, bits)]
    if head in ('def', 'defi', 'defb'):
        d = enc_dst(ops[0], ps, major, 0)
        if head == 'def':
            if len(ops) != 5:
                raise AsmError('def takes a register and 4 values')
            vals = [float_bits(x) for x in ops[1:]]
            return [co | OP_DEF | (5 << 24 if major >= 2 else 0), d] + vals
        if head == 'defi':
            if len(ops) != 5:
                raise AsmError('defi takes a register and 4 values')
            vals = [int(x, 0) & 0xffffffff for x in ops[1:]]
            return [co | OP_DEFI | (5 << 24 if major >= 2 else 0), d] + vals
        if len(ops) != 2 or ops[1] not in ('true', 'false'):
            raise AsmError('defb takes a register and true or false')
        return [co | OP_DEFB | (2 << 24 if major >= 2 else 0), d, 1 if ops[1] == 'true' else 0]
    base, bits, cmp = take_mods(head)
    if base not in NAMES:
        raise AsmError('unknown instruction %r' % head)
    op = NAMES[base]
    if opname(op, ps, major, minor, {'texldp': 1, 'texldb': 2}.get(base, 0)) != base:
        raise AsmError('%s is not an instruction of %s_%d_%d' % (base, 'ps' if ps else 'vs', major, minor))
    ctrl = {'texldp': 1, 'texldb': 2}.get(base, 0)
    if op in CMP_OPS:
        if not cmp:
            raise AsmError('%s needs a comparison' % base)
        ctrl = cmp
    elif cmp:
        raise AsmError('%s takes no comparison' % base)
    toks = []
    srcs = ops
    if OPS[op][1]:
        if not ops:
            raise AsmError('%s needs a destination' % base)
        toks.append(enc_dst(ops[0], ps, major, bits))
        srcs = ops[1:]
    elif bits:
        raise AsmError('%s takes no modifiers' % base)
    want = OPS[op][2]
    if op == OP_TEX:
        want = 0 if base == 'tex' else 1 if major == 1 else 2
    elif op == OP_TEXCOORD:
        want = 1 if base == 'texcrd' else 0
    elif op == 37:
        want = 3 if major == 2 else 1
    if len(srcs) != want:
        raise AsmError('%s takes %d source%s' % (base, want, '' if want == 1 else 's'))
    for a in srcs:
        toks += enc_src(a, ps, major)
    n = len(toks) if major >= 2 else 0
    if n > 15:
        raise AsmError('too many operands')
    return [co | op | ctrl << 16 | n << 24] + toks


def assemble(text):
    """text -> bytecode (bytes)"""
    ver = None
    out = []
    for no, raw in enumerate(text.splitlines(), 1):
        line = raw.split(';')[0].split('//')[0].strip()
        if not line:
            continue
        try:
            if ver is None:
                m = re.match(r'^(vs|ps)_(\d)_(\d|x|sw)$', line)
                if not m:
                    raise AsmError('the first line must be the version (vs_2_0, ps_1_1, ...)')
                ps = m.group(1) == 'ps'
                major = int(m.group(2))
                minor = {'x': 1, 'sw': 0xff}.get(m.group(3)) if not m.group(3).isdigit() else int(m.group(3))
                ver = (0xffff0000 if ps else 0xfffe0000) | major << 8 | minor
                out.append(ver)
                continue
            out += assemble_line(line, ps, major, minor)
        except (AsmError, ValueError, IndexError) as e:
            raise AsmError('line %d: %s: %s' % (no, raw.strip(), e))
    if ver is None:
        raise AsmError('empty shader')
    out.append(OP_END)
    return struct.pack('<%dI' % len(out), *out)


# ------------------------------------------------------------------ files
def fnv1a64(b):
    h = 0xcbf29ce484222325
    for x in b:
        h = ((h ^ x) * 0x100000001b3) & 0xffffffffffffffff
    return h


def read_pak(path):
    d = open(path, 'rb').read()
    o = 0
    res = []
    while o < len(d):
        n = struct.unpack_from('<I', d, o)[0]
        if o + 4 + n > len(d):
            raise AsmError('%s: entry at %d runs past the end' % (path, o))
        res.append(d[o + 4:o + 4 + n])
        o += 4 + n
    return res


def read_any(path):
    """[(label, bytecode)] for a .pak or a raw shader"""
    d = open(path, 'rb').read()
    if len(d) >= 4 and struct.unpack_from('<I', d)[0] >> 16 in (0xfffe, 0xffff):
        return [(os.path.basename(path), d)]
    return [('%s #%d (0x%x)' % (os.path.basename(path), i, i), b) for i, b in enumerate(read_pak(path))]


DEFAULT_GAME = os.path.expanduser('~/.local/share/stranglehold-native/game')


def paks(game):
    out = []
    for name in ('vshaders.pak', 'pshaders.pak'):
        p = os.path.join(game, name)
        if not os.path.exists(p):
            raise AsmError('%s not found' % p)
        out += [(name, i, b) for i, b in enumerate(read_pak(p))]
    return out


def cmd_selftest(game):
    bad = 0
    n = 0
    for name, i, b in paks(game):
        n += 1
        try:
            lines, nraw = disassemble(b)
            back = assemble('\n'.join(lines))
        except AsmError as e:
            print('%s #%d: %s' % (name, i, e))
            bad += 1
            continue
        if back != b or nraw:
            bad += 1
            print('%s #%d: %s%s' % (name, i, 'differs after reassembly' if back != b else '',
                                    ' %d raw' % nraw if nraw else ''))
    print('%d shaders: %s' % (n, 'all byte-identical after disassembly and reassembly' if not bad else '%d FAILED' % bad))
    return 1 if bad else 0


def cmd_dump(game, out):
    os.makedirs(out, exist_ok=True)
    index = []
    for name, i, b in paks(game):
        h = '%016x' % fnv1a64(b)
        ext = '.psh' if name.startswith('p') else '.vsh'
        lines, nraw = disassemble(b)
        hdr = ['; %s #%d (0x%x), %d bytes, fnv1a64 %s' % (name, i, i, len(b), h),
               '; (copy to data/shaders/ keeping the name to replace it: tools/d3dasm.py)']
        with open(os.path.join(out, h + ext), 'w') as f:
            f.write('\n'.join(hdr + lines) + '\n')
        index.append('%s%s  %s #%d (0x%x)  %s' % (h, ext, name, i, i, lines[0]))
    with open(os.path.join(out, 'index.txt'), 'w') as f:
        f.write('\n'.join(index) + '\n')
    print('%d shaders written to %s' % (len(index), out))


def cmd_ctable(src, out):
    files = sorted(f for f in (os.listdir(src) if os.path.isdir(src) else [])
                   if f.endswith(('.vsh', '.psh')))
    body = ['/* Generated by tools/d3dasm.py ctable from %s/: the shaders runtime/d3d9.c puts in place' % src,
            '   of the game\'s (option shaders = improved), by the FNV-1a 64 hash of the bytecode replaced. */',
            '#include <stdint.h>', '']
    hashes, names, lens = [], [], []
    for k, f in enumerate(files):
        m = re.match(r'^([0-9a-f]{16})\.(vsh|psh)$', f)
        if not m:
            raise AsmError('%s/%s: a replacement is named <16 hex digits of the hash>.vsh or .psh' % (src, f))
        code = assemble(open(os.path.join(src, f)).read())
        ver = struct.unpack_from('<I', code)[0] >> 16
        if (ver == 0xffff) != (m.group(2) == 'psh'):
            raise AsmError('%s/%s: a %s shader in a .%s file' % (src, f, 'pixel' if ver == 0xffff else 'vertex', m.group(2)))
        w = struct.unpack('<%dI' % (len(code) // 4), code)
        body.append('static const uint32_t sub_%d[] = {' % k)
        for j in range(0, len(w), 8):
            body.append('    ' + ' '.join('0x%08xu,' % x for x in w[j:j + 8]))
        body.append('};')
        hashes.append('0x%sull' % m.group(1))
        names.append('"%s"' % f)
        lens.append('%du' % len(w))
    n = len(files)
    def arr(decl, items):
        return '%s[%d] = {%s};' % (decl, max(n, 1), ', '.join(items) if items else '0')
    body += ['const int shader_sub_count = %d;' % n,
             arr('const uint64_t shader_sub_hash', hashes),
             arr('const uint32_t shader_sub_len', lens),
             arr('const uint32_t *const shader_sub_code', ['sub_%d' % k for k in range(n)]),
             arr('const char *const shader_sub_name', names), '']
    text = '\n'.join(body)
    old = open(out).read() if os.path.exists(out) else None
    if old != text:
        with open(out, 'w') as f:
            f.write(text)


def main(a):
    if not a:
        print(__doc__)
        return 2
    cmd, args = a[0], a[1:]
    if cmd == 'dis':
        sel = [int(x, 0) for x in args[1:]]
        items = read_any(args[0])
        for k, (label, b) in enumerate(items):
            if sel and k not in sel:
                continue
            lines, _ = disassemble(b)
            if len(items) > 1:
                print('; ==== %s, %d bytes, fnv1a64 %016x' % (label, len(b), fnv1a64(b)))
            print('\n'.join(lines))
        return 0
    if cmd == 'asm':
        code = assemble(open(args[0]).read())
        if '-o' in args:
            open(args[args.index('-o') + 1], 'wb').write(code)
        print('%d bytes, fnv1a64 %016x' % (len(code), fnv1a64(code)))
        return 0
    if cmd == 'hash':
        for label, b in read_any(args[0]):
            print('%016x  %s' % (fnv1a64(b), label))
        return 0
    if cmd == 'dump':
        cmd_dump(args[0] if args else DEFAULT_GAME, args[1] if len(args) > 1 else 'work/shaders')
        return 0
    if cmd == 'selftest':
        return cmd_selftest(args[0] if args else DEFAULT_GAME)
    if cmd == 'ctable':
        cmd_ctable(args[0], args[1])
        return 0
    print(__doc__)
    return 2


if __name__ == '__main__':
    try:
        sys.exit(main(sys.argv[1:]))
    except AsmError as e:
        sys.exit('d3dasm: %s' % e)
