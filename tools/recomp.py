"""Static recompiler: a 32-bit MSVC Windows program and its DLLs -> C.

    recomp.py <game folder> <build config .py> <output dir>

The build config's MODULES (the program first, then the DLLs translated with it) are read from
the game folder. Writes the generated C into the output dir: funcs.h (prototypes), code_NNN.c
(one C function per function, at its original address), table.c (address -> function table,
the modules, the import slots the runtime fills: its own functions or another module's),
stubs.c (imports the runtime does not provide) and image.h (layout constants). Nothing from the
program's code or data is copied: the runtime maps the user's own files at start.
"""
import sys, os, time, hashlib, importlib.util
sys.path.insert(0, os.path.dirname(__file__))
from image import Image, Module, Decoder
from discover import Discovery
from codegen import FuncGen, Program, Unsupported, import_symbol

LINES_PER_FILE = 30000


class out_file:
    """A text file written only when its contents change, so make rebuilds only what changed."""
    def __init__(self, path):
        self.path, self.parts = path, []

    def write(self, t):
        self.parts.append(t)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        if exc[0]:
            return False
        text = ''.join(self.parts)
        try:
            with open(self.path, newline='\n') as fh:
                if fh.read() == text:
                    return False
        except OSError:
            pass
        with open(self.path, 'w', newline='\n') as fh:
            fh.write(text)
        return False


def load_config(path):
    spec = importlib.util.spec_from_file_location('buildcfg', path)
    cfg = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(cfg)
    return cfg


def main():
    game, cfgpath, out = sys.argv[1:4]
    cfg = load_config(cfgpath)
    mods = []
    for name, fn, want in cfg.MODULES:
        path = os.path.join(game, fn)
        sha = hashlib.sha256(open(path, 'rb').read()).hexdigest()
        if sha != want:
            sys.exit('%s is not the file the %s build describes (sha256 %s)' % (path, cfg.NAME, sha))
        mods.append(Module(name, path))
    # byte changes to the code before translation (builds/*.py CODE_PATCHES: va -> (original hex,
    # new hex)); the original bytes are checked so a patch never lands on other code
    for va, (orig, new) in sorted(getattr(cfg, 'CODE_PATCHES', {}).items()):
        orig, new = bytes.fromhex(orig.replace(' ', '')), bytes.fromhex(new.replace(' ', ''))
        m = next(m for m in mods if m.lo <= va < m.hi)
        o = va - m.pe.base
        if len(orig) != len(new) or bytes(m.pe.image[o:o + len(orig)]) != orig:
            sys.exit('code patch at %08x: the original bytes differ' % va)
        m.pe.image[o:o + len(new)] = new
    pe = Image(mods)
    dec = Decoder(pe)
    iat = {va: (dll, name) for dll, name, va in pe.imports}
    t0 = time.time()
    disc = Discovery(pe, dec, {va: n for va, (d, n) in iat.items()}, cfg.NORETURN_IMPORTS,
                     cfg.EXTRA_ENTRIES, cfg.JUMP_TABLES, cfg.NOT_ENTRIES)
    funcs = disc.run(pe.seeds(), pe.data_code_pointers())
    empty = sorted(e for e, f in funcs.items() if e not in f.insns)
    for e in empty:
        print('  %08x: no code at this entry (%s): a stub that stops with a message' % (e, funcs[e].errors[:1]))
    for m in mods:
        n = sum(1 for e in funcs if m.lo <= e < m.hi)
        print('  %s: %d functions' % (m.name, n))
    errs = [(e, a, m) for e, f in funcs.items() for a, m in f.errors]
    print('discovery: %d functions, %d instructions, %d errors (%.1fs)' % (
        len(funcs), sum(len(f.insns) for f in funcs.values()), len(errs), time.time() - t0))
    for e in errs[:20]:
        print('  error in %08x at %08x: %s' % e)
    for l in disc.log:
        print('  ' + l)

    prog = Program(pe, funcs, disc.noreturn, iat, cfg.NORETURN_IMPORTS,
                   getattr(cfg, 'NONLOCAL_JUMPS', []))
    prog.set_tables(getattr(cfg, 'TABLES', []))
    prog.hooks = dict(getattr(cfg, 'HOOKS', {}))
    prog.icall_fpu = dict(getattr(cfg, 'ICALL_FPU', {}))
    prog.inline = dict(getattr(cfg, 'INLINE', {}))
    prog.set_consts(getattr(cfg, 'CONSTS', {}))
    prog.mem_overrides = {a: n for n, (a, orig) in getattr(cfg, 'MEM_OVERRIDES', {}).items()}
    prog.flag_interface()
    prog.fpu_interface()
    os.makedirs(out, exist_ok=True)

    # ---------------------------------------------------------------- functions
    warns = []
    chunks, cur, curlines = [], [], 0
    nlines = 0
    for e in sorted(funcs):
        g = FuncGen(prog, funcs[e])
        try:
            text = g.gen()
        except Unsupported as ex:
            warns.append('%08x: %s' % (e, ex))
            text = ('/* %08x: not translated: %s */\nvoid %s(CPU *c)\n{\n    unsupported(c, 0x%xu, "%s");\n}\n'
                    % (e, ex, prog.fname(e), e, str(ex).replace('"', "'")))
        warns.extend('%08x: %s' % (e, w) for w in g.warn)
        n = text.count('\n')
        nlines += n
        cur.append(text)
        curlines += n
        if curlines > LINES_PER_FILE:
            chunks.append(cur)
            cur, curlines = [], 0
    if cur:
        chunks.append(cur)
    for k, ch in enumerate(chunks):
        with out_file(os.path.join(out, 'code_%03d.c' % k)) as fh:
            fh.write('/* Generated from the user\'s game files by tools/recomp.py. Do not edit or share. */\n')
            fh.write('#include "cpu.h"\n#include "funcs.h"\n\n')
            fh.write('\n'.join(ch))
    for fn in os.listdir(out):
        if fn.startswith('code_') and fn.endswith('.c') and int(fn[5:8]) >= len(chunks):
            os.remove(os.path.join(out, fn))
    with out_file(os.path.join(out, 'funcs.h')) as fh:
        fh.write('#pragma once\n#include "cpu.h"\n')
        for e in sorted(funcs):
            fh.write('void %s(CPU *c);\n' % prog.fname(e))
        for va, (dll, name) in sorted(iat.items()):
            fh.write('void %s(CPU *c);\n' % import_symbol(dll, name))
        for t in prog.tables:
            fh.write('extern uint32_t reloc_%s, limit_%s;\n' % (t['name'], t['name']))
        for name in sorted(getattr(cfg, 'CONSTS', {})):
            fh.write('extern uint32_t rc_%s;\n' % name)
        for name in sorted(getattr(cfg, 'MEM_OVERRIDES', {})):
            fh.write('extern uint32_t rm_%s;   /* the operand\'s address; the runtime may change it */\n' % name)
        for a, h in sorted(prog.hooks.items()):
            fh.write('extern int (*hook_%s)(CPU *c);   /* at %08x; returns 1 if it did the call itself */\n' % (h, a))
            fh.write('void %s_orig(CPU *c);\n' % prog.fname(a))

    # ---------------------------------------------------------------- tables
    with out_file(os.path.join(out, 'table.c')) as fh:
        fh.write('/* Generated by tools/recomp.py */\n#include "cpu.h"\n#include "funcs.h"\n#include "runtime.h"\n\n')
        fh.write('const FuncEntry func_table[] = {\n')
        for e in sorted(funcs):
            fh.write('    {0x%08xu, %s},\n' % (e, prog.fname(e)))
        fh.write('};\nconst int func_count = %d;\n\n' % len(funcs))
        fh.write('const ImportEntry import_table[] = {\n')
        for va, (dll, name) in sorted(iat.items()):
            fh.write('    {0x%08xu, "%s", "%s", %s},\n' % (va, dll, name, import_symbol(dll, name)))
        fh.write('};\nconst int import_count = %d;\n\n' % len(iat))
        # import slots another translated module provides: the runtime writes the address in
        fh.write('const LinkEntry link_table[] = {\n')
        for va, t in sorted(pe.resolved.items()):
            fh.write('    {0x%08xu, 0x%08xu},   /* %s */\n' % (va, t, pe.resolved_name[va]))
        fh.write('};\nconst int link_count = %d;\n\n' % len(pe.resolved))
        fh.write('const ModuleEntry module_table[] = {\n')
        for m, (name, fn, want) in zip(mods, cfg.MODULES):
            p = m.pe
            texts = ', '.join('{0x%08xu, 0x%08xu}' % t for t in m.text[:8])
            fh.write('    {"%s", "%s", "%s", 0x%08xu, 0x%08xu, 0x%08xu, %d, {%s}},\n' % (
                name, fn, want, p.base, p.size_of_image, p.entry if p.entry_rva else 0, len(m.text[:8]), texts))
        fh.write('};\nconst int module_count = %d;\n\n' % len(mods))
        # movable tables: base and capacity the code uses, changed by the runtime for mods
        for t in prog.tables:
            fh.write('uint32_t reloc_%s = 0x%08xu, limit_%s = %du;\n' % (t['name'], t['base'], t['name'], t['count']))
        for a, h in sorted(prog.hooks.items()):
            fh.write('int (*hook_%s)(CPU *c);\n' % h)
        for name, (a, orig) in sorted(getattr(cfg, 'MEM_OVERRIDES', {}).items()):
            fh.write('uint32_t rm_%s = 0x%08xu;\n' % (name, orig))
        # literals the runtime may change (CONSTS), at their original values
        for name, d in sorted(getattr(cfg, 'CONSTS', {}).items()):
            fh.write('uint32_t rc_%s = 0x%xu;\n' % (name, d['value']))
        fh.write('const TableEntry table_list[] = {\n')
        for t in prog.tables:
            fh.write('    {"%s", &reloc_%s, &limit_%s, 0x%08xu, %du, %du},\n' % (
                t['name'], t['name'], t['name'], t['base'], t['entry'], t['count']))
        fh.write('    {0}\n};\n')
    # imports the runtime does not implement: weak stubs that say so. In a file of their own: on
    # Windows, GNU ld relocates a reference to a weak function from the object that defines it
    # wrongly once a strong definition replaces it (table.c's import_table pointed into other code).
    with out_file(os.path.join(out, 'stubs.c')) as fh:
        fh.write('/* Generated by tools/recomp.py */\n#include "cpu.h"\n#include "funcs.h"\n#include "runtime.h"\n\n')
        done = set()
        for va, (dll, name) in sorted(iat.items()):
            sym = import_symbol(dll, name)
            if sym in done:
                continue
            done.add(sym)
            fh.write('__attribute__((weak)) void %s(CPU *c) { missing_import(c, "%s", "%s"); }\n'
                     % (sym, dll, name.replace('\\', '\\\\')))
    # x87 instructions whose results differ between CPU emulators and real hardware: the
    # reference runs (Unicorn) compute these on the host CPU instead (tests/fncheck.c)
    trans = {}
    for f in funcs.values():
        for a, i in f.insns.items():
            if i.mn in ('fsin', 'fcos', 'fsincos', 'fptan', 'fpatan', 'fyl2x', 'fyl2xp1', 'f2xm1'):
                trans[a] = i.raw[-1]
    with out_file(os.path.join(out, 'x87_special.c')) as fh:
        fh.write('/* Generated by tools/recomp.py: x87 transcendental instructions (address, opcode byte) */\n')
        fh.write('#include <stdint.h>\nconst uint32_t x87_special_addr[] = {\n')
        fh.write(''.join('    0x%08xu,\n' % a for a in sorted(trans)))
        fh.write('};\nconst uint8_t x87_special_op[] = {\n')
        fh.write(''.join('    0x%02x,\n' % trans[a] for a in sorted(trans)))
        fh.write('};\nconst int x87_special_count = %d;\n' % len(trans))
        # SSE reciprocal approximations (rcpss/rcpps/rsqrtss/rsqrtps): hardware and emulators
        # round them differently; the reference runs do them on the host CPU too
        approx = {a: len(i.raw) for f in funcs.values() for a, i in f.insns.items()
                  if i.mn in ('rcpss', 'rcpps', 'rsqrtss', 'rsqrtps')}
        fh.write('const uint32_t sse_special_addr[] = {\n')
        fh.write(''.join('    0x%08xu,\n' % a for a in sorted(approx)) or '    0,\n')
        fh.write('};\nconst uint8_t sse_special_len[] = {\n')
        fh.write(''.join('    %d,\n' % approx[a] for a in sorted(approx)) or '    0,\n')
        fh.write('};\nconst int sse_special_count = %d;\n' % len(approx))
    main = mods[0].pe
    with out_file(os.path.join(out, 'image.h')) as fh:
        fh.write('#pragma once\n')
        fh.write('#define IMAGE_BASE 0x%08xu\n' % main.base)
        fh.write('#define IMAGE_SIZE 0x%08xu\n' % main.size_of_image)
        fh.write('#define IMAGE_ENTRY 0x%08xu\n' % main.entry)
        fh.write('#define STACK_RESERVE 0x%08xu\n' % main.stack_reserve)
        fh.write('#define MODULE_COUNT %d\n' % len(mods))
        fh.write('#define BUILD_NAME "%s"\n' % cfg.NAME)
        for name, a in sorted(getattr(cfg, 'DATA', {}).items()):
            fh.write('#define GAME_%s 0x%08xu\n' % (name.upper(), a))
    with out_file(os.path.join(out, 'warnings.txt')) as fh:
        fh.write('\n'.join(warns) + '\n')
    print('generated %d functions, %d lines in %d files, %d warnings (%.1fs)' % (
        len(funcs), nlines, len(chunks), len(warns), time.time() - t0))
    for w in warns[:30]:
        print('  ' + w)


if __name__ == '__main__':
    main()
