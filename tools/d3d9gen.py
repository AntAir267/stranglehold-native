"""Generate the Direct3D 9 bridge's method wrappers from DXVK's d3d9.h.

    d3d9gen.py <d3d9.h> <output .c>

Each COM interface the program uses becomes a ComClass whose vtable entries are runtime
functions: they read the program's 32-bit stdcall arguments, convert what differs between the
program's 32-bit and the host's 64-bit layouts (interface pointers, window handles, shared
handles), call DXVK's object through its vtable by index, and wrap returned interfaces. Methods
needing more (locks, present parameters, QueryInterface, Release, ...) are written by hand in
runtime/d3d9.c and named in SPECIAL; the generated table points at those.
"""
import re
import sys

IFACES = ['IDirect3D9', 'IDirect3DDevice9', 'IDirect3DStateBlock9', 'IDirect3DSwapChain9',
          'IDirect3DResource9', 'IDirect3DVertexDeclaration9', 'IDirect3DVertexShader9',
          'IDirect3DPixelShader9', 'IDirect3DBaseTexture9', 'IDirect3DTexture9',
          'IDirect3DVolumeTexture9', 'IDirect3DCubeTexture9', 'IDirect3DVertexBuffer9',
          'IDirect3DIndexBuffer9', 'IDirect3DSurface9', 'IDirect3DVolume9', 'IDirect3DQuery9']
# interfaces the program can hold (the abstract bases only appear as parameter types)
CLASSES = [i for i in IFACES if i not in ('IDirect3DResource9', 'IDirect3DBaseTexture9')]

# hand-written in runtime/d3d9.c: "Iface::Method" or "*::Method" for every interface
SPECIAL = {
    '*::QueryInterface', '*::AddRef', '*::Release',
    'IDirect3D9::CreateDevice', 'IDirect3D9::GetAdapterMonitor', 'IDirect3D9::RegisterSoftwareDevice',
    'IDirect3DDevice9::Reset', 'IDirect3DDevice9::CreateAdditionalSwapChain',
    'IDirect3DDevice9::GetCreationParameters', 'IDirect3DDevice9::Present',
    'IDirect3DDevice9::GetNPatchMode',
    'IDirect3DSwapChain9::Present', 'IDirect3DSwapChain9::GetPresentParameters',
    'IDirect3DVertexBuffer9::Lock', 'IDirect3DVertexBuffer9::Unlock',
    'IDirect3DIndexBuffer9::Lock', 'IDirect3DIndexBuffer9::Unlock',
    'IDirect3DTexture9::LockRect', 'IDirect3DTexture9::UnlockRect',
    'IDirect3DCubeTexture9::LockRect', 'IDirect3DCubeTexture9::UnlockRect',
    'IDirect3DVolumeTexture9::LockBox', 'IDirect3DVolumeTexture9::UnlockBox',
    'IDirect3DSurface9::LockRect', 'IDirect3DSurface9::UnlockRect',
    'IDirect3DVolume9::LockBox', 'IDirect3DVolume9::UnlockBox',
    'IDirect3DSurface9::GetDC', 'IDirect3DSurface9::ReleaseDC',
    'IDirect3DSurface9::GetContainer', 'IDirect3DVolume9::GetContainer',
    '*::SetPrivateData', '*::GetPrivateData',
}

INTERFACE_TYPES = set(IFACES) | {'IUnknown'}


def parse(header):
    s = open(header).read()
    out = {}
    for name in IFACES:
        m = re.search(r'DECLARE_INTERFACE_\(%s,\s*(\w+)\)\s*\{(.*?)\n\};' % name, s, re.S)
        if not m:
            sys.exit('%s not found' % name)
        base, body = m.group(1), m.group(2)
        body = re.sub(r'/\*.*?\*/', '', body, flags=re.S)
        methods = []
        pat = r'STDMETHOD(?:_\(\s*([^,]+?)\s*,\s*(\w+)\s*\)|\((\w+)\))\s*\((.*?)\)\s*PURE;'
        for r, n1, n2, args in re.findall(pat, body.replace('\n', ' '), re.S):
            ret, mname = r or 'HRESULT', n1 or n2
            params = []
            for a in args.split(','):
                a = a.replace('THIS_', '').replace('THIS', '').strip()
                if not a or a == 'void':
                    continue
                mm = re.match(r'^(.*?)(\w+)\s*$', a)
                if not mm:                      # no name, ends in '*'
                    typ, pname = a, 'a%d' % len(params)
                else:
                    typ, pname = mm.group(1).strip(), mm.group(2)
                if not typ:                     # an unnamed parameter
                    typ, pname = pname, 'a%d' % len(params)
                typ = typ.replace('struct ', '')
                const = 'const' in typ.split()
                stars = typ.count('*')
                bare = typ.replace('const', '').replace('*', '').strip()
                params.append(dict(type=bare, stars=stars, const=const, name=pname))
            methods.append(dict(ret=ret.strip(), name=mname, params=params))
        out[name] = (base, methods)
    return out


def all_methods(defs, name):
    base, methods = defs[name]
    if methods and methods[0]['name'] == 'QueryInterface':
        return methods                  # the header lists the inherited methods again: complete
    if base in defs:
        return all_methods(defs, base) + methods
    if base == 'IUnknown':
        return [dict(ret='HRESULT', name='QueryInterface', params=[]),
                dict(ret='ULONG', name='AddRef', params=[]),
                dict(ret='ULONG', name='Release', params=[])] + \
               [m for m in methods if m['name'] not in ('QueryInterface', 'AddRef', 'Release')]
    return methods


def ctype(p):
    t = ('const ' if p['const'] else '') + p['type'] + ' ' + '*' * p['stars']
    return t.strip()


def gen_method(iface, idx, m):
    """C body for one method, or None if it cannot be generated (then it must be SPECIAL)"""
    pre, args, post, decl = [], [], [], []
    for k, p in enumerate(m['params']):
        a = 'ARG(%d)' % (k + 1)
        t, st = p['type'], p['stars']
        if t in INTERFACE_TYPES and st == 1:
            args.append('(void *)d3d_host_or_null(%s)' % a)
        elif t in INTERFACE_TYPES and st == 2:
            decl.append('void *o%d = NULL;' % k)
            args.append('%s ? &o%d : NULL' % (a, k))
            wrapper = 'd3d_wrap_new' if m['name'].startswith('Create') else 'd3d_wrap'
            post.append('if (%s) W32(%s, o%d ? %s(DK_%s, o%d) : 0);' % (a, a, k, wrapper, t, k))
        elif t == 'HWND' and st == 0:
            args.append('d3d_hwnd(%s)' % a)
        elif t == 'HANDLE' and st == 1:
            pre.append('if (%s) W32(%s, 0);' % (a, a))
            args.append('NULL')
        elif t in ('HDC', 'HMONITOR') or (t == 'void' and st == 2):
            return None
        elif t in ('D3DPRESENT_PARAMETERS', 'D3DLOCKED_RECT', 'D3DLOCKED_BOX',
                   'D3DDEVICE_CREATION_PARAMETERS') and st:
            return None
        elif st >= 1 or t in ('REFGUID', 'REFIID'):
            args.append('(%s)(uintptr_t)%s' % (ctype(p), a))
        elif t == 'float':
            args.append('d3d_argf(c, %d)' % (k + 1))
        else:
            args.append('(%s)%s' % (ctype(p), a))
    def sigtype(p):
        if p['type'] in INTERFACE_TYPES:
            return 'void ' + '*' * p['stars']
        return ctype(p)
    sig = ', '.join(['void *'] + [sigtype(p) for p in m['params']])
    sig = sig.replace('HWND', 'void *')
    ret = m['ret']
    if ret == 'float' or ret == 'FLOAT':
        return None
    rtype = 'void' if ret == 'void' else ret
    call = '((%s (*)(%s))VT(self)[%d])(%s)' % (rtype, sig, idx, ', '.join(['self'] + args))
    body = ['    void *self = d3d_host(ARG(0));'] + ['    ' + d for d in decl] + ['    ' + p for p in pre]
    if ret == 'void':
        body.append('    %s;' % call)
        body += ['    ' + p for p in post]
        body.append('    return 0;')
    else:
        body.append('    uint32_t r = (uint32_t)%s;' % call)
        body += ['    ' + p for p in post]
        if ret == 'HRESULT':
            body.append('    if ((int32_t)r < 0 && r != 0x8876086au) LOG(LOG_D3D, "%s::%s -> %%08x", r);' % (iface, m['name']))
        body.append('    return r;')
    return body


def arg_kinds(m):
    """one letter a parameter, for the draw-stream hash (runtime/drawhash.c): s a value, i an
    interface, - left out (out pointers, unknown data), m v M l r c a matrix, viewport,
    material, light, rect or clip plane read through the pointer, R rects (count in the
    argument before), f n b shader constants (float / int / BOOL, count in the argument after),
    u DrawPrimitiveUP vertices, U DrawIndexedPrimitiveUP indices and vertices"""
    fixed = {'D3DMATRIX': 'm', 'D3DVIEWPORT9': 'v', 'D3DMATERIAL9': 'M', 'D3DLIGHT9': 'l', 'RECT': 'r'}
    k = ''
    for i, p in enumerate(m['params']):
        t, st = p['type'], p['stars']
        if st == 0 and t not in ('REFGUID', 'REFIID'):
            k += 's'
        elif t in INTERFACE_TYPES and st == 1:
            k += 'i'
        elif not p['const'] or st != 1:
            k += '-'
        elif t in fixed:
            k += fixed[t]
        elif t == 'D3DRECT':
            k += 'R'
        elif t == 'float' and m['name'] == 'SetClipPlane':
            k += 'c'
        elif t in ('float', 'int', 'BOOL', 'WINBOOL') and m['name'].startswith('Set') and 'ShaderConstant' in m['name']:
            k += {'float': 'f', 'int': 'n', 'BOOL': 'b', 'WINBOOL': 'b'}[t]
        elif m['name'] == 'DrawPrimitiveUP' and t == 'void':
            k += 'u'
        elif m['name'] == 'DrawIndexedPrimitiveUP' and t == 'void':
            k += 'U'
        else:
            k += '-'
    return k


# what a method's output parameters hold, in the program's (32-bit) layout: their sizes, so a
# recording of the calls (runtime/devrec.c) can keep every byte they write, changed or not
OUT_SIZES = {'D3DADAPTER_IDENTIFIER9': 1100, 'D3DCAPS9': 304, 'D3DCLIPSTATUS9': 8,
             'D3DDEVICE_CREATION_PARAMETERS': 16, 'D3DDISPLAYMODE': 16, 'D3DGAMMARAMP': 1536,
             'D3DINDEXBUFFER_DESC': 20, 'D3DLIGHT9': 104, 'D3DLOCKED_BOX': 12, 'D3DLOCKED_RECT': 8,
             'D3DMATERIAL9': 68, 'D3DMATRIX': 64, 'D3DPRESENT_PARAMETERS': 56, 'D3DRASTER_STATUS': 8,
             'D3DSURFACE_DESC': 32, 'D3DVERTEXBUFFER_DESC': 24, 'D3DVIEWPORT9': 24,
             'D3DVOLUME_DESC': 28, 'DWORD': 4, 'UINT': 4, 'HANDLE': 4, 'HDC': 4, 'RECT': 16}


def out_sizes(m):
    """per parameter: the bytes an output pointer gets (0: an input, or not known)"""
    r = []
    for p in m['params']:
        if p['const'] or not p['stars'] or (p['type'] in INTERFACE_TYPES and p['stars'] == 1):
            r.append(0)
        elif p['stars'] == 2:
            r.append(4)                 # (an interface or a pointer handed back)
        else:
            r.append(OUT_SIZES.get(p['type'], 0))
    return r


def main():
    header, out = sys.argv[1:3]
    defs = parse(header)
    lines = ['/* Generated by tools/d3d9gen.py from DXVK\'s d3d9.h. Do not edit. */',
             '#include "d3d9_bridge.h"', '']
    tables = []
    for iface in CLASSES:
        methods = all_methods(defs, iface)
        names = []
        for idx, m in enumerate(methods):
            key, star = '%s::%s' % (iface, m['name']), '*::%s' % m['name']
            nargs = 1 + len(m['params'])
            if key in SPECIAL or star in SPECIAL:
                fn = 'sp_%s_%s' % (iface, m['name']) if key in SPECIAL else 'sp_any_%s' % m['name']
                lines.append('void %s(CPU *c);' % fn)
                names.append((fn, m['name']))
                continue
            body = gen_method(iface, idx, m)
            fn = 'm_%s_%s' % (iface, m['name'])
            if body is None:
                lines.append('static void %s(CPU *c) { d3d_unsupported(c, "%s::%s", %d); }' % (fn, iface, m['name'], nargs))
            else:
                lines.append('METHOD(%s, %d)' % (fn, nargs))
                lines.append('{')
                lines += body
                lines.append('}')
            names.append((fn, m['name']))
        if iface == 'IDirect3DDevice9':
            lines.append('const char *const d3d_device_methods[] = {%s, NULL};' % ', '.join('"%s"' % n for f, n in names))
            lines.append('const char *const d3d_device_argkinds[] = {%s};' % ', '.join('"%s"' % arg_kinds(m) for m in methods))
        lines.append('static void (*const fns_%s[])(CPU *) = {%s};' % (iface, ', '.join(f for f, n in names)))
        lines.append('static const char *const names_%s[] = {%s};' % (iface, ', '.join('"%s"' % n for f, n in names)))
        lines.append('')
        tables.append((iface, len(names)))
    specs = []
    for iface in CLASSES:
        for m in all_methods(defs, iface):
            if m['name'] in ('QueryInterface', 'AddRef', 'Release'):
                continue
            o = out_sizes(m)
            if any(o):
                specs.append('    {"%s::%s", {%s}},' % (iface, m['name'], ', '.join(str(x) for x in o[:10])))
    lines.append('/* methods with output parameters: their sizes by parameter (runtime/devrec.c) */')
    lines.append('const struct D3DOutSpec { const char *name; unsigned short out[10]; } d3d_out_specs[] = {')
    lines += specs
    lines.append('    {0, {0}}};')
    lines.append('ComClass d3d_classes[DK_COUNT];')
    lines.append('void d3d_classes_init(void)')
    lines.append('{')
    for iface, n in tables:
        lines.append('    com_class_init(&d3d_classes[DK_%s], "%s", %d, fns_%s, names_%s);' % (iface, iface, n, iface, iface))
    lines.append('}')
    with open(out, 'w', newline='\n') as fh:
        fh.write('\n'.join(lines) + '\n')
    # the kinds, for the hand-written side
    with open(out.replace('.c', '.h'), 'w', newline='\n') as fh:
        fh.write('/* Generated by tools/d3d9gen.py. */\n#pragma once\nenum {\n')
        for iface in IFACES:
            fh.write('    DK_%s,\n' % iface)
        fh.write('    DK_IUnknown,\n    DK_COUNT\n};\n')


if __name__ == '__main__':
    main()
