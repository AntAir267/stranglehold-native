"""Read, dump and patch Midway's ScreenStudio data: the WooScreenSet object (class
WooScreenEngine.WooScreenSet) that holds every menu/HUD screen of a ui_* package.

Format [verified 2026-10-27 by the exe's load-time pointer fix-up code, 0x9b7dc0 (SSET),
0x9b7ab0 (SDAT), 0x9b7730 (object tree), 0x9b7650/0x9b76e0 (handlers and their argument
lists), 0x9b79d0/0x9b78d0 (animations); every pointer below is a byte offset that the loader
turns into an address by adding the base named]:

UE3 export: a tagged property block (ReferencedTextures: array of Texture2D, RawData: array of
bytes) then None. RawData is the ScreenStudio blob. All u32/u16/float fields follow the
package byte order (PC little-endian, Xbox 360 big-endian); the tags read 'SSET', 'SDAT',
'OBJ ', 'GROP', 'POLY', 'TEXT' as big-endian u32 (on PC the bytes are reversed, 'TESS').

SSET (base = blob start): +0 tag, +4 u16 version (0), +6 u8, +7 u8 flags (bit 0 = fixed up),
  +8 ptr screen-offset table, +0xc u32 [guess: unused], +0x10 screen count,
  +0x14 ptr[count] screen names (C strings), then the names, then the table, then the screens.
SDAT, one per screen (base = SDAT start): +0 tag, +4 u16 version (1), +6 u16, +8 u32,
  +0xc ptr root node, +0x10 ptr animation table, +0x14 ptr string table
  (count, ptr[count] to C strings). Names, image names, localization keys, action strings
  and expression bytecode are all indices into this table.
Node (common): +0 tag, +4 i32 name (string index, -1 none), +8 u32, +0xc u32 flags
  (bit 0 = visible [verified: PC hides the Accept footer object with flags 2, the 360 has 3]).
  OBJ / GROP (0x20 bytes): +0x10 ptr handler list (count, ptr[count]), +0x14 ptr transform
    block (20 floats: [0..2] ?, [3..5] scale, [6..8] position, [9..11] ?, [12..15] colour
    multiply RGBA, [16..19] ? [guess from values]), +0x18 ptr child list (count, ptr[count]),
    +0x1c ptr X block (u32, ptr argument list) [guess: object parameters].
  POLY (0x74 bytes): +0x10 4 vertices (x, y, z floats), +0x40 4 colours (u32), +0x50 4 UVs,
    +0x70 image name (string index; the Texture2D of that name).
  TEXT (0x34 bytes): +0x10 text (string index, a localization key), +0x14 font (string index),
    +0x18 float size, +0x1c float, +0x20 float, +0x24 float width, +0x28 float height,
    +0x2c colour u32, +0x30 u32 flags (alignment).
Handler (an event's script): +0 u32 event id, +4 entry count, +8 count of top-level entries
  (entries 0..n-1 run in order), then entries of 16 bytes: +0 u32 (-1), +4 u32 action op,
  +8 u16 first child entry, +0xa u16 child count, +0xc ptr argument list.
  Children are run by If/Else/IfVar actions (0x9b3690 runs a child range; an If that ran its
  block disables the Else/ElseIf that follows it).
Argument list: count, ptr[count] to {u32 type, value}: type 4/7 string index, 5/6/10/11 pointer
  (6 = node, 10 = node for visibility), 9 = id (sound/var/event, 12 bytes: id, -1 cache),
  1 = int, 0 = int/bool, 2 = float (raw bits).
Expression strings (args of If 0x40f/0x410, Question etc., evaluator 0x9c1c30/0x9c0ce0):
  byte classes by top 2 bits: 01 = variable slot (low 5 bits; bit 5: a property byte follows;
  as the first byte it is the assignment target, slot 0 = just evaluate), 10 = operator
  (see EXPR_OPS: 0x81 +, 0x82 -, 0x8f..0x94 compares, 0x98 '(' 0x99 ')', 0x86 end ...), 11 = literal (0xc0 u8, 0xc1 u16, 0xc2 i32, 0xc3 float, values
  big-endian in both versions). The cooker folds platform tests to literals: '@\\xc0\\x00' is 0
  and '@\\xc0\\x01' is 1 [verified: m_Settings Control Config: PC If(0),If(0),If(1) ->
  m_pcKeyboardConfig, 360 If(1),If(0),If(0) -> m_controllerSetup].

    screenset.py dump <package> <outbase>          JSON + text tree (outbase.json, outbase.txt)
    screenset.py roundtrip <package>               re-serialise, compare byte for byte
    screenset.py diff <pkgA> <pkgB> <out.txt>      per-screen structural differences
    screenset.py rows <package> <screen>           focusable objects, widgets, up/down links
    screenset.py patch <package> <out> <name>...   apply named patches (see PATCHES) to a copy
"""
import json
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import upkparse  # noqa: E402

# Engine action ops (factory 0x9b8ea0, class names from RTTI; exec = vtable slot 2).
ENGINE_OPS = {
    1000: 'SetFocus', 1001: 'Action', 1002: 'Visible', 1003: 'OpenScreen', 1004: 'SendObjectEvent',
    1007: 'SetScreenVisible', 1014: 'SetFocus', 1016: 'PlayAnim', 1017: 'StopAnim',
    1018: 'UserConfirm', 1019: 'PlayAnim', 1020: 'SetAnimSpeed', 1021: 'PlayAnimUntil',
    1022: 'SnapAnim', 1023: 'BlockEventsUntil', 1024: 'WaitAnim', 1026: 'SetForward',
    1027: 'Enable', 1028: 'UserConfirm', 1029: 'UserConfirm', 1030: 'UserConfirm',
    1031: 'UserConfirm', 1032: 'SendObjectEvent', 1033: 'PlayAnimUntil', 1034: 'Math',
    1035: 'IfFloat', 1036: 'ElseIfFloat', 1038: 'Math', 1039: 'If', 1040: 'ElseIf',
    1047: 'Sound', 1050: 'Question', 1056: 'WaitAnim', 1064: 'SnapAnim', 1065: 'Else',
    1071: 'SetFocus', 1072: 'SetScreenVisible', 1073: 'Enable', 1074: 'SnapAnim',
    1075: 'RemoveData', 1076: 'SendObjectEvent', 1077: 'EventDataCheck', 1090: 'LevelLoad',
    1092: 'LevelSendEvent',
}
# Game-side ops (FWooScreenClient action handler 0x6aba80; switch tables 0x6ad94c, 0x6ad964).
GAME_OPS = {
    3004: 'Command', 5002: 'PlaySound', 11000: 'SetVar', 11001: 'IfVar',
    11017: 'IfConfigBool', 11079: 'Nop',
}
for _o in (11047, 11086, 11088):
    GAME_OPS[_o] = 'Nop'
OP_1003_NOTE = 'OpenScreen [guess: arg is the screen path]'
CMP_OPS = ['==', '!=', '>', '>=', '<', '<=']   # 0x9b7320
# integer evaluator 0x9c0ce0, binary ops by table 0x9c15c0 [verified: disassembly of each case]
EXPR_OPS = {0x81: '+', 0x82: '-', 0x83: '*', 0x84: '/', 0x85: '/u', 0x86: ',', 0x87: '&&',
            0x88: '||', 0x89: '<<', 0x8a: '>>', 0x8b: '&', 0x8c: '|', 0x8d: '^', 0x8e: '~',
            0x8f: '<=', 0x90: '<', 0x91: '==', 0x92: '!=', 0x93: '>=', 0x94: '>', 0x95: '!',
            0x96: '?', 0x97: ':', 0x98: '(', 0x99: ')'}


def op_name(op):
    return ENGINE_OPS.get(op) or GAME_OPS.get(op) or 'op%d' % op


def expr_text(b):
    """Readable form of an expression string (bytes up to the end of the string)."""
    out = []
    i = 0
    while i < len(b):
        c = b[i]
        cls = c >> 6
        if cls == 1:
            s = 'v%d' % (c & 0x1f)
            if c & 0x20 and i + 1 < len(b):
                i += 1
                s += '.p%d' % b[i]
            out.append(s)
        elif cls == 2:
            out.append(EXPR_OPS.get(c, 'op%02x' % c))
        elif cls == 3:
            k = c & 7
            if i + 1 >= len(b) and k < 4:
                break
            n = [1, 2, 4, 4][k] if k < 4 else 0
            v = b[i + 1:i + 1 + n] + b'\0' * (n - len(b[i + 1:i + 1 + n]))
            if k == 0:
                out.append(str(v[0]))
            elif k == 1:
                out.append(str(struct.unpack('>H', v)[0]))
            elif k == 2:
                out.append(str(struct.unpack('>i', v)[0]))
            elif k == 3:
                out.append('%g' % struct.unpack('>f', v)[0])
            i += n
        elif c == 0 and i == len(b) - 1:
            break
        else:
            out.append('?%02x' % c)
        i += 1
    if out and out[0].startswith('v') and len(out) > 1:
        tgt = out[0]
        return ('eval ' if tgt == 'v0' else tgt + ' = ') + ' '.join(out[1:])
    return ' '.join(out)


class Screen:
    """One SDAT block. Pointers are offsets from its start. Edits go to self.b."""

    def __init__(self, name, data, e):
        self.name = name
        self.b = bytearray(data)
        self.e = e
        self.dirty = False

    # --- primitive access
    def u32(self, o):
        return struct.unpack_from(self.e + 'I', self.b, o)[0]

    def i32(self, o):
        return struct.unpack_from(self.e + 'i', self.b, o)[0]

    def u16(self, o):
        return struct.unpack_from(self.e + 'H', self.b, o)[0]

    def f32(self, o):
        return struct.unpack_from(self.e + 'f', self.b, o)[0]

    def put32(self, o, v):
        struct.pack_into(self.e + ('i' if v < 0 else 'I'), self.b, o, v)
        self.dirty = True

    def put16(self, o, v):
        struct.pack_into(self.e + 'H', self.b, o, v)
        self.dirty = True

    def tag(self, o):
        return struct.pack('>I', self.u32(o)).decode('latin-1')

    def cstr_bytes(self, o):
        return bytes(self.b[o:self.b.index(b'\0', o)])

    def expr_bytes(self, o):
        """An expression string read the way the evaluator does: literal payload bytes may be
        0, so it ends at a 0 byte in operand/operator position (0x9c0ce0's loop test)."""
        i = o
        first = True
        while i < len(self.b):
            c = self.b[i]
            if c == 0:
                break
            if c >> 6 == 1 and c & 0x20 and not first:
                i += 1                              # property byte
            elif c >> 6 == 1 and c & 0x20 and first:
                i += 1
            elif c >> 6 == 3 and (c & 7) < 4:
                i += [1, 2, 4, 4][c & 7]
            i += 1
            first = False
        return bytes(self.b[o:i])

    # --- tables
    def strtab(self):
        st = self.u32(0x14)
        return [self.u32(st + 4 + 4 * k) for k in range(self.u32(st))]

    def strings(self):
        return [self.cstr_bytes(p) for p in self.strtab()]

    def string(self, k):
        t = self.strtab()
        return self.cstr_bytes(t[k]) if 0 <= k < len(t) else None

    def append(self, data, align=4):
        while len(self.b) % align:
            self.b.append(0)
        o = len(self.b)
        self.b += data
        self.dirty = True
        return o

    def add_strings(self, strs):
        """Add strings (bytes, NUL added); returns their indices. Existing equal strings are
        reused. The string table is moved to the end of the screen (old copy left unused)."""
        cur = self.strings()
        idx, new = [], []
        for s in strs:
            if s in cur:
                idx.append(cur.index(s))
            elif s in new:
                idx.append(len(cur) + new.index(s))
            else:
                new.append(s)
                idx.append(len(cur) + len(new) - 1)
        if not new:
            return idx
        offs = self.strtab()
        for s in new:
            # 4-aligned and zero-padded like the cooker's pool: an expression ending in a 1-byte
            # literal of 0 ('@\xc0') needs the payload 0 and then the terminator
            d = s + b'\0'
            d += b'\0' * (-len(d) % 4 or (4 if d[-2:-1] == b'\xc0' else 0))
            offs.append(self.append(d, 4))
        tab = struct.pack(self.e + 'I', len(offs)) + b''.join(struct.pack(self.e + 'I', o) for o in offs)
        self.put32(0x14, self.append(tab))
        return idx

    def new_args(self, args):
        """Append an argument list [(type, value), ...]; returns its offset."""
        items = []
        for t, v in args:
            body = struct.pack(self.e + 'I', t) + struct.pack(self.e + ('i' if v < 0 else 'I'), v)
            if t == 9:
                body += struct.pack(self.e + 'i', -1)
            items.append(body)
        n = len(items)
        head = 4 + 4 * n
        offs, pos = [], 0
        for it in items:
            offs.append(pos)
            pos += len(it)
        while len(self.b) % 4:
            self.b.append(0)
        base = len(self.b)
        blob = struct.pack(self.e + 'I', n) + b''.join(
            struct.pack(self.e + 'I', base + head + o) for o in offs) + b''.join(items)
        return self.append(blob)

    # --- tree walking
    def args(self, p):
        out = []
        if not p:
            return out
        for k in range(self.u32(p)):
            q = self.u32(p + 4 + 4 * k)
            t = self.u32(q)
            v = self.i32(q + 4)
            a = {'off': q, 'type': t, 'value': v}
            if t in (4, 7):
                s = self.string(v)
                a['string'] = s.decode('latin-1') if s is not None else None
                if s and len(s) > 1 and s[0] >> 6 == 1 and any(c >= 0x80 for c in s[1:]):
                    a['expr'] = expr_text(self.expr_bytes(self.strtab()[v]))
            elif t == 2:
                a['float'] = struct.unpack('<f', struct.pack('<I', v & 0xffffffff))[0]
            out.append(a)
        return out

    def handler(self, h):
        cnt = self.u32(h + 4)
        ent = []
        for i in range(cnt):
            q = h + 0xc + 16 * i
            op = self.u32(q + 4)
            ent.append({'off': q, 'a': self.i32(q), 'op': op, 'name': op_name(op),
                        'first': self.u16(q + 8), 'nchild': self.u16(q + 0xa),
                        'args': self.args(self.u32(q + 0xc))})
        return {'off': h, 'event': self.u32(h), 'top': self.u32(h + 8), 'entries': ent}

    def node(self, p, depth=0):
        t = self.tag(p)
        n = {'off': p, 'tag': t.strip(), 'name': self.i32(p + 4), 'u8': self.u32(p + 8),
             'flags': self.u32(p + 0xc)}
        if n['name'] >= 0:
            s = self.string(n['name'])
            n['name_s'] = s.decode('latin-1') if s is not None else None
        if t in ('OBJ ', 'GROP'):
            hl, tr, cl, xb = (self.u32(p + o) for o in (0x10, 0x14, 0x18, 0x1c))
            if tr:
                n['xform'] = [round(self.f32(tr + 4 * i), 4) for i in range(20)]
            n['handlers'] = [self.handler(self.u32(hl + 4 + 4 * k)) for k in range(self.u32(hl))] if hl else []
            if xb:
                n['x'] = {'off': xb, 'u0': self.u32(xb), 'args': self.args(self.u32(xb + 4))}
            n['children'] = [self.node(self.u32(cl + 4 + 4 * k), depth + 1)
                             for k in range(self.u32(cl))] if cl else []
        elif t == 'POLY':
            n['verts'] = [round(self.f32(p + 0x10 + 4 * i), 3) for i in range(12)]
            n['colors'] = [self.b[p + 0x40 + 4 * i:p + 0x44 + 4 * i].hex() for i in range(4)]  # bytes
            n['uv'] = [round(self.f32(p + 0x50 + 4 * i), 4) for i in range(8)]
            k = self.u32(p + 0x70)
            s = self.string(k)
            n['image'] = s.decode('latin-1') if s is not None else k
        elif t == 'TEXT':
            k1, k2 = self.u32(p + 0x10), self.u32(p + 0x14)
            s1, s2 = self.string(k1), self.string(k2)
            n['text'] = s1.decode('latin-1') if s1 is not None else k1
            n['font'] = s2.decode('latin-1') if s2 is not None else k2
            n['size'] = [round(self.f32(p + o), 3) for o in (0x18, 0x1c, 0x20, 0x24, 0x28)]
            n['color'] = self.b[p + 0x2c:p + 0x30].hex()          # 4 bytes, same in both orders
            n['align'] = self.b[p + 0x30:p + 0x34].hex()          # 4 bytes [guess: alignment flags]
        return n

    def anims(self):
        a = self.u32(0x10)
        if not a:
            return []
        out = []
        for i in range(self.u32(a)):
            q = a + 4 + 0x18 * i
            out.append({'off': q, 'head': [self.u32(q + 4 * j) for j in range(4)],
                        'targets': self.u32(q + 0x10), 'anim': self.u32(q + 0x14)})
        return out

    def tree(self):
        return {'name': self.name, 'version': self.u16(4), 'root': self.node(self.u32(0xc)),
                'anims': self.anims(), 'strings': [s.decode('latin-1') for s in self.strings()]}

    def walk_entries(self, n=None):
        """Yield (node, handler, entry_index, entry) over the screen."""
        if n is None:
            n = self.node(self.u32(0xc))
        for h in n.get('handlers', []):
            for i, e in enumerate(h['entries']):
                yield n, h, i, e
        for c in n.get('children', []):
            yield from self.walk_entries(c)

    def walk_nodes(self, n=None):
        if n is None:
            n = self.node(self.u32(0xc))
        yield n
        for c in n.get('children', []):
            yield from self.walk_nodes(c)


class ScreenSet:
    def __init__(self, path):
        self.path = path
        self.pkg = upkparse.load(path)
        self.e = self.pkg.e
        exs = [x for x in self.pkg.exports if self.pkg.class_name(x) == 'WooScreenSet']
        assert len(exs) == 1, 'expected one WooScreenSet export, found %d' % len(exs)
        self.ex = exs[0]
        d = self.pkg.export_data(self.ex)
        tag = b'SSET' if self.e == '>' else b'TESS'
        i = d.find(tag)
        # property tag of RawData: name(8) type(8) size(4) index(4), then count(4) == bytes
        self.raw_count_pos = i - 4
        self.raw_size_pos = i - 12
        n = struct.unpack_from(self.e + 'I', d, i - 4)[0]
        assert struct.unpack_from(self.e + 'I', d, i - 12)[0] == n + 4
        self.prefix = d[:i - 12]
        self.suffix = d[i + n:]
        self.raw = d[i:i + n]
        r = self.raw
        U = lambda o: struct.unpack_from(self.e + 'I', r, o)[0]
        self.count = U(0x10)
        tab = U(8)
        self.head_end = U(tab)                       # first screen
        self.screens = []
        offs = [U(tab + 4 * k) for k in range(self.count)]
        for k in range(self.count):
            nm = r[U(0x14 + 4 * k):r.index(b'\0', U(0x14 + 4 * k))].decode('latin-1')
            end = offs[k + 1] if k + 1 < self.count else len(r)
            self.screens.append(Screen(nm, r[offs[k]:end], self.e))
        self.table = tab

    def screen(self, name):
        for s in self.screens:
            if s.name.lower() == name.lower():
                return s
        raise KeyError(name)

    def build_raw(self):
        out = bytearray(self.raw[:self.head_end])
        for k, s in enumerate(self.screens):
            while len(out) % 4:
                out.append(0)
            struct.pack_into(self.e + 'I', out, self.table + 4 * k, len(out))
            out += s.b
        return bytes(out)

    def build_export(self):
        raw = self.build_raw()
        # prefix ends at the RawData tag's size field; then array index, element count, bytes
        return self.prefix + struct.pack(self.e + 'I', len(raw) + 4) + \
            self.pkg.d[self.ex.offset + len(self.prefix) + 4:self.ex.offset + len(self.prefix) + 8] + \
            struct.pack(self.e + 'I', len(raw)) + raw + self.suffix

    def build_package(self):
        """Whole package: unchanged size -> written in place; otherwise the screen set is
        appended at the end of the file and only its export entry (size, offset) changes."""
        exd = self.build_export()
        d = bytearray(self.pkg.d)
        if len(exd) == self.ex.size:
            d[self.ex.offset:self.ex.offset + self.ex.size] = exd
            return bytes(d)
        off = len(d)
        d += exd
        struct.pack_into(self.e + 'ii', d, self.ex.size_pos, len(exd), off)
        return bytes(d)


# ---------------------------------------------------------------- text dump
def fmt_args(args):
    out = []
    for a in args:
        t = a['type']
        if 'expr' in a:
            out.append('expr{%s}' % a['expr'])
        elif 'string' in a:
            out.append(repr(a['string']))
        elif t in (5, 6, 10, 11):
            out.append(a['value'] if isinstance(a['value'], str) else '@%#x' % a['value'])
        elif t == 9:
            out.append('#%d' % a['value'])
        elif t == 2:
            out.append('%gf' % a['float'])
        else:
            out.append(str(a['value']))
    return ', '.join(out)


def entry_line(e):
    s = '%s(%s)' % (e['name'], fmt_args(e['args']))
    if e['op'] == 11001 and len(e['args']) == 3:
        a = e['args']
        cmp = CMP_OPS[a[1]['value']] if 0 <= a[1]['value'] < 6 else '?'
        s = 'IfVar(var#%d %s %d)' % (a[0]['value'], cmp, a[2]['value'])
    return s


def text_tree(sc, out):
    t = sc.tree()
    out.append('=== screen %s (strings %d)' % (sc.name, len(t['strings'])))

    def node(n, ind):
        nm = (' "%s"' % n['name_s']) if 'name_s' in n else ''
        line = '%s%s @%#x%s flags=%d' % (ind, n['tag'], n['off'], nm, n['flags'])
        if n['tag'] == 'POLY':
            xs, ys = n['verts'][0::3], n['verts'][1::3]
            line += ' image=%r rect=(%g,%g)-(%g,%g)' % (n['image'], min(xs), min(ys), max(xs), max(ys))
        elif n['tag'] == 'TEXT':
            line += ' text=%r font=%r size=%s color=%s' % (n['text'], n['font'], n['size'], n['color'])
        elif 'xform' in n:
            x = n['xform']
            line += ' pos=(%g,%g,%g) scale=(%g,%g)' % (x[6], x[7], x[8], x[3], x[4])
        out.append(line)
        if 'x' in n:
            out.append('%s  X(%d: %s)' % (ind, n['x']['u0'], fmt_args(n['x']['args'])))
        for h in n.get('handlers', []):
            out.append('%s  on event %d (handler @%#x, %d entries)' % (ind, h['event'], h['off'], len(h['entries'])))
            ents = h['entries']

            def run(i, ind2, seen):
                if i in seen or i >= len(ents):
                    return
                seen.add(i)
                e = ents[i]
                out.append('%s[%d] %s' % (ind2, i, entry_line(e)))
                for c in range(e['first'], e['first'] + e['nchild']):
                    run(c, ind2 + '    ', seen)
            seen = set()
            for i in range(h['top']):
                run(i, ind + '    ', seen)
            for i in range(len(ents)):
                if i not in seen:
                    run(i, ind + '    (unreached) ', seen)
        for c in n.get('children', []):
            node(c, ind + '  ')
    node(t['root'], '  ')


def dump(path, outbase):
    ss = ScreenSet(path)
    js = {'package': os.path.basename(path), 'endian': 'big' if ss.e == '>' else 'little',
          'screens': [s.tree() for s in ss.screens]}
    with open(outbase + '.json', 'w') as f:
        json.dump(js, f, indent=1)
    out = ['package %s, %d screens' % (os.path.basename(path), len(ss.screens))]
    for s in ss.screens:
        text_tree(s, out)
    with open(outbase + '.txt', 'w') as f:
        f.write('\n'.join(out) + '\n')
    return ss


# ---------------------------------------------------------------- diff
def shape(sc):
    """Comparable lines for a screen (offsets left out, pointers as node ordinals)."""
    lines = []
    nodes = list(sc.walk_nodes())
    ordn = {n['off']: k for k, n in enumerate(nodes)}

    def norm(e):
        e = dict(e)
        e['args'] = [dict(a, value='node%s' % ordn.get(a['value'], '?%x' % a['value']))
                     if a['type'] in (5, 6, 10, 11) else a for a in e['args']]
        return e
    for n in nodes:
        d = {k: v for k, v in n.items() if k not in ('off', 'children', 'handlers', 'x')}
        lines.append(('node', json.dumps(d, sort_keys=True)))
        for h in n.get('handlers', []):
            lines.append(('handler', 'event %d top %d n %d' % (h['event'], h['top'], len(h['entries']))))
            for i, e in enumerate(h['entries']):
                e = norm(e)
                lines.append(('entry', '[%d] %s ch=%d+%d' % (i, entry_line(e), e['first'], e['nchild'])))
    return lines


def diff(pa, pb, outp):
    import difflib
    A, B = ScreenSet(pa), ScreenSet(pb)
    out = []
    bn = {s.name.lower(): s for s in B.screens}
    for s in A.screens:
        o = bn.pop(s.name.lower(), None)
        if o is None:
            out.append('only in A: ' + s.name)
            continue
        la = [re.sub('(?i)xenon', '', x[0] + ' ' + x[1]) for x in shape(s)]
        lb = [re.sub('(?i)xenon', '', x[0] + ' ' + x[1]) for x in shape(o)]
        if la == lb:
            continue
        out.append('=== %s' % s.name)
        for l in difflib.unified_diff(la, lb, lineterm='', n=1):
            if not l.startswith(('---', '+++')):
                out.append(l)
    for k in bn:
        out.append('only in B: ' + bn[k].name)
    with open(outp, 'w') as f:
        f.write('\n'.join(out) + '\n')


# ---------------------------------------------------------------- focus rows
NAV_EVENTS = {1006: 'up', 1007: 'down', 1008: 'left', 1009: 'right'}


def rows(path, screen):
    """Focusable objects of a screen: name, label, widget (X block), and where up/down/left/right
    move the focus (SetFocus targets in the 1006-1009 handlers)."""
    ss = ScreenSet(path)
    sc = ss.screen(screen)
    out = []
    for n in sc.walk_nodes():
        ev = {h['event']: h for h in n.get('handlers', [])}
        if not (set(ev) & set(NAV_EVENTS) or 'x' in n):
            continue
        label = next((m['text'] for m in _subtree(n) if m['tag'] == 'TEXT'), '')
        nav = []
        for e, nm in NAV_EVENTS.items():
            if e in ev:
                t = [x['args'][1]['value'] for x in ev[e]['entries']
                     if x['op'] in (1000, 1014, 1071) and len(x['args']) > 1]
                nav.append('%s=%s' % (nm, ('%#x' % t[0]) if t else 'none'))
        xs = ''
        if 'x' in n:
            xs = '%s(%s)' % (n['x']['u0'].to_bytes(4, 'big').decode('latin-1'), fmt_args(n['x']['args']))
        out.append('%#7x %-26s flags=%#-6x %-26s %s %s' % (n['off'], n.get('name_s', ''), n['flags'],
                                                         label[:26], ' '.join(nav), xs))
    return out


# ---------------------------------------------------------------- patches
LIT0, LIT1 = b'@\xc0', b'@\xc0\x01'          # cooked platform tests: false / true
PAD_VAR = 9999   # [guess] a free screen var id the runtime would set to 1 while a pad is in use


def _branch_ifs(sc, target_pc, target_x):
    """The If entries of an accept handler that pick target_x (Xbox branch) / target_pc."""
    found = []
    for n, h, i, e in sc.walk_entries():
        if e['op'] != 1039:
            continue
        ents = h['entries']
        kids = [ents[c] for c in range(e['first'], e['first'] + e['nchild'])]
        for k in kids:
            if k['op'] == 1003 and k['args'] and k['args'][0].get('string'):
                found.append((h, i, e, k['args'][0]['string']))
    xb = [f for f in found if f[3].endswith(target_x)]
    pc = [f for f in found if f[3].endswith(target_pc)]
    return xb, pc


def _set_lit(sc, e, lit):
    k = sc.add_strings([lit])[0]
    sc.put32(e['args'][0]['off'] + 4, k)


def _to_ifvar(sc, e, value):
    """Turn an If(literal) entry into IfVar(PAD_VAR == value) keeping its child block."""
    sc.put32(e['off'] + 4, 11001)
    sc.put32(e['off'] + 0xc, sc.new_args([(9, PAD_VAR), (1, 0), (1, value)]))


def patch_settings_route(ss, mode):
    """Every copy of the Settings menu (m_Settings, and the copies inside m_AudioSettings,
    m_controllerSetup, m_MultiplayerSettings): the Control Config entry opens the 360 layout
    screen m_controllerSetup instead of m_pcKeyboardConfig.
    mode 'always': swap the cooked literals; 'pad': IfVar(PAD_VAR == 1 / == 0) on the Xbox and
    PC branches (PAD_VAR must then be set by the runtime, see work/screenset_notes.md)."""
    n = 0
    for sc in ss.screens:
        xb, pc = _branch_ifs(sc, 'm_pcKeyboardConfig', 'm_controllerSetup')
        if not pc:
            continue
        assert len(pc) == 1 and len(xb) == 2, (sc.name, len(xb), len(pc))
        xe, pe = xb[0][2], pc[0][2]                 # first If naming the 360 screen = Xbox branch
        if mode == 'always':
            _set_lit(sc, xe, LIT1)
            _set_lit(sc, pe, LIT0)
        else:
            _to_ifvar(sc, xe, 1)
            _to_ifvar(sc, pe, 0)
        n += 1
    print('controlconfig-%s: %d menus' % (mode, n))


BUTTON_EVENTS = range(2102, 2121)   # footer "this object is button X" events (0x9b5a90 table)


def _subtree(n):
    yield n
    for c in n.get('children', []):
        yield from _subtree(c)


FOOTER_SCREENS = ('m_chapterSelect', 'm_MainMenu', 'm_Settings', 'm_extras')


def patch_footers_visible(ss):
    """Show the A/B footer objects the PC data hides where the 360 shows them [verified: footer
    comparison PC vs 360 ui_shell, work/screenset_notes.md]: OBJ with button event 2102 (B) or
    2105 (A) in FOOTER_SCREENS, flags bit 0 set; their Nav_A/Nav_B image too. (The PC's Y
    'storage location' footer in m_MainMenu stays hidden.) The images still need the runtime's
    pad-mode bypass of the poly-draw blacklist (FWooScreenPoly draw 0x6dc770)."""
    n_obj = n_poly = 0
    for name in FOOTER_SCREENS:
        sc = ss.screen(name)
        for n in sc.walk_nodes():
            if n['tag'] != 'OBJ' or n['flags'] & 1:
                continue
            ev = {h['event'] for h in n.get('handlers', [])}
            if not ev & {2102, 2105} or ev & {2104, 2113}:
                continue
            polys = [m for m in _subtree(n) if m['tag'] == 'POLY' and
                     m.get('image') in ('Generic_Elements-Nav_A', 'Generic_Elements-Nav_B')]
            if not polys:
                continue
            sc.put32(n['off'] + 0xc, n['flags'] | 1)
            n_obj += 1
            for m in polys:
                if not m['flags'] & 1:
                    sc.put32(m['off'] + 0xc, m['flags'] | 1)
                    n_poly += 1
    print('footers-visible: %d objects, %d images' % (n_obj, n_poly))


def patch_pause_vibration(ss):
    """ig_PauseMenu: show the Controller Vibration row the PC hides (flags 0x8000 -> 0x8003)."""
    sc = ss.screen('ig_PauseMenu')
    for n in sc.walk_nodes():
        if n.get('name_s') == 'Toggle_ControllerVibration':
            sc.put32(n['off'] + 0xc, n['flags'] | 3)
            print('pause-vibration: row @%#x flags %#x -> %#x' % (n['off'], n['flags'], n['flags'] | 3))


def clone_poly(sc, src_off, image=None):
    """Append a copy of a POLY node (0x74 bytes), optionally with another image; returns offset."""
    data = bytearray(sc.b[src_off:src_off + 0x74])
    o = sc.append(bytes(data))
    if image is not None:
        k = sc.add_strings([image.encode('latin-1')])[0]
        sc.put32(o + 0x70, k)
    return o


def add_children(sc, grp_off, new):
    """Append node offsets to an OBJ/GROP child list (the list is copied to the end)."""
    cl = sc.u32(grp_off + 0x18)
    kids = [sc.u32(cl + 4 + 4 * k) for k in range(sc.u32(cl))] if cl else []
    kids += new
    blob = struct.pack(sc.e + 'I', len(kids)) + b''.join(struct.pack(sc.e + 'I', k) for k in kids)
    sc.put32(grp_off + 0x18, sc.append(blob))


def patch_video_apply_icon(ss):
    """m_pcVideoOptions: give the Apply footer the backing + Xbox Start image like the other
    footers (Apply = event 2110 = Start, 0x9b5a90) and move its text right like Reset's."""
    sc = ss.screen('m_pcVideoOptions')
    apply_obj = reset_obj = None
    for n in sc.walk_nodes():
        ev = [h['event'] for h in n.get('handlers', [])]
        if n['tag'] == 'OBJ' and 2110 in ev:
            apply_obj = n
        if n['tag'] == 'OBJ' and 2113 in ev:
            reset_obj = n
    assert apply_obj and reset_obj
    rgrp = reset_obj['children'][0]
    back, icon = [c for c in rgrp['children'] if c['tag'] == 'POLY'][:2]
    agrp = apply_obj['children'][0]
    text = [c for c in agrp['children'] if c['tag'] == 'TEXT'][0]
    p1 = clone_poly(sc, back['off'])
    p2 = clone_poly(sc, icon['off'], 'Generic_Elements-Xbox_Start')
    # the new polys go first (drawn under the text), as in the other footers
    cl = sc.u32(agrp['off'] + 0x18)
    old = [sc.u32(cl + 4 + 4 * k) for k in range(sc.u32(cl))]
    blob = struct.pack(sc.e + 'I', len(old) + 2) + b''.join(struct.pack(sc.e + 'I', k) for k in [p1, p2] + old)
    sc.put32(agrp['off'] + 0x18, sc.append(blob))
    rtext = [c for c in rgrp['children'] if c['tag'] == 'TEXT'][0]
    for o in (0x18, 0x1c):                          # text x, y offset as Reset's (24, 1)
        sc.b[text['off'] + o:text['off'] + o + 4] = sc.b[rtext['off'] + o:rtext['off'] + o + 4]
    print('video-apply-icon: polys @%#x @%#x added to @%#x' % (p1, p2, agrp['off']))


PATCHES = {
    'controlconfig-always': lambda ss: patch_settings_route(ss, 'always'),
    'controlconfig-pad': lambda ss: patch_settings_route(ss, 'pad'),
    'footers-visible': patch_footers_visible,
    'pause-vibration': patch_pause_vibration,
    'video-apply-icon': patch_video_apply_icon,
}


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    cmd = argv[1]
    if cmd == 'dump':
        dump(argv[2], argv[3])
    elif cmd == 'roundtrip':
        ss = ScreenSet(argv[2])
        raw_ok = ss.build_raw() == ss.raw
        for s in ss.screens:
            s.dirty = True
        pk = ss.build_package()
        print('raw identical:', raw_ok, ' export identical:', ss.build_export() == ss.pkg.export_data(ss.ex),
              ' package identical:', pk == ss.pkg.d)
    elif cmd == 'rows':
        print('\n'.join(rows(argv[2], argv[3])))
    elif cmd == 'diff':
        diff(argv[2], argv[3], argv[4])
    elif cmd == 'patch':
        ss = ScreenSet(argv[2])
        for name in argv[4:]:
            PATCHES[name](ss)
        open(argv[3], 'wb').write(ss.build_package())
        print('wrote', argv[3])
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
