"""Name a calltree.c tree (tools/calltree.sh): function offsets -> game addresses (F_xxxxxxxx) and
Direct3D methods (D3D:Method); writes the indented tree and, beside it (.pkl), its rows
(depth, name, calls) for scripts.

    calltree.py PROGRAM RAW OUT
"""
import pickle, re, subprocess, sys
prog, raw, out = sys.argv[1:4]
syms = {}
for l in subprocess.run(['nm', prog], capture_output=True, text=True).stdout.split('\n'):
    p = l.split()
    if len(p) == 3 and p[1] in 'tT':
        syms[int(p[0], 16)] = p[2]
def name(off):
    n = syms.get(off, '?%x' % off)
    m = re.match(r'F_([0-9a-f]{8})(_orig)?$', n)
    if m:
        return m.group(1).lstrip('0')
    return re.sub(r'^(m|sp)_IDirect3DDevice9_', 'D3D:', n)
rows, prev = [], None
for l in open(raw):
    d = len(l) - len(l.lstrip(' '))
    a, n = l.split()
    nm = name(int(a, 16))
    # a hooked function shows as F_x then F_x_orig: one row
    if prev and prev[1] == nm and d == prev[0] + 1:
        continue
    prev = (d, nm, int(n))
    rows.append(prev)
# (re-indent after dropping the hooked functions' second rows)
fixed, stack = [], []
for d, nm, n in rows:
    while stack and stack[-1][0] >= d:
        stack.pop()
    fixed.append((len(stack), nm, n))
    stack.append((d, nm))
with open(out, 'w') as f:
    for d, nm, n in fixed:
        f.write('%s%s %d\n' % (' ' * d, nm, n))
pickle.dump(fixed, open(out + '.pkl', 'wb'))
print('%s: %d rows' % (out, len(fixed)))
