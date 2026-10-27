"""Controller parity in the user's own Startup package (DESIGN.md section 4): each PC control config
the game pushes by name also includes its Xbox 360 twin, as the 360's own XBoxConfig includes the
keyboard sets beside the pad's - so keyboard, mouse and a pad all work at once, the pad exactly as
on the 360 (its config objects are the 360's, byte for byte: DESIGN.md section 7).

    upkpatch.py <game folder> <user folder> [--map MAP]

Also writes <user>/StrangleholdGame/Config/PC/Cooked/Coalesced.ini: the user's config with the
force-feedback manager the 360 creates ([Engine.PlayerController]
ForceFeedbackManagerClassName=WinDrv.XnaForceFeedbackManager, commented out on PC), so rumble can
be played (runtime/game.c). --map (tests only) makes the game start in that map.

Reads <game>/StrangleholdGame/CookedPC/Startup_<lang>.xxx (LZO-compressed), and writes the patched
package to <user>/StrangleholdGame/CookedPC/ with its .uncompressed_size (the runtime reads the
user folder first). A changed object's data goes at the end of the package and only its export
table entry (size, offset) is rewritten, so nothing else moves. Nothing from the game is kept in
this repository; the result is made from the user's file on their machine.
"""
import ctypes
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
import upkdecomp  # noqa: E402
import upkparse  # noqa: E402

BLOCK = 0x20000
RENAME = 100

# (PC config, Xbox configs it should also include). The game pushes these by short name
# [verified: exe strings UIStartButton, UISelectButton, ScreenEngineConfig formatted with the
# player number at 0x6a62c0; PCConfig at 0x6a7240; the rest pushed by scripts' PushControlConfig],
# and only the first loaded object of a name is used, which is the PC one.
PAD_SETS = ['Movement', 'Targeting', 'XBoxActions', 'VehicleActions']
MERGES = [('PCConfig', PAD_SETS)]
for n in range(4):
    MERGES += [('UIStartButton%d' % n, ['UIStartButton%d' % n]),
               ('UISelectButton%d' % n, ['UISelectButton%d' % n]),
               ('UI%d' % n, ['UI%d' % n])]
for name in ('CinemaCameraConfig', 'MoviePlayerConfig', 'FreeCameraConfig', 'GuitarCaseConfig',
             'HelicopterTurretConfig', 'MexicanStandoffConfig'):
    MERGES.append((name, [name]))


def tags(pk, d, pos):
    """the tagged properties from pos: [(name, type, start, value start, size)] up to None"""
    out = []
    e = pk.e
    while True:
        ni = struct.unpack_from(e + 'i', d, pos)[0]
        name = pk.names[ni]
        if name == 'None':
            return out, pos
        ti = struct.unpack_from(e + 'i', d, pos + 8)[0]
        size = struct.unpack_from(e + 'i', d, pos + 16)[0]
        typ = pk.names[ti]
        vstart = pos + 24
        if typ == 'BoolProperty':
            vstart += 1                     # (the value byte; size says 0) [guess: v375 layout]
            end = vstart
        else:
            end = vstart + size
        out.append((name, typ, pos, vstart, size))
        pos = end


def add_includes(pk, data, refs):
    """data with each export index in refs added to its Includes array (created if missing)"""
    e = pk.e
    props, none_at = tags(pk, data, 4)       # (after the net index)
    inc = [p for p in props if p[0] == 'Includes']
    if inc:
        _, _, start, vstart, size = inc[0]
        count = struct.unpack_from(e + 'i', data, vstart)[0]
        have = [struct.unpack_from(e + 'i', data, vstart + 4 + 4 * k)[0] for k in range(count)]
        new = [r for r in refs if r not in have]
        if not new:
            return data
        body = struct.pack(e + 'i', count + len(new)) + data[vstart + 4:vstart + size] + \
            b''.join(struct.pack(e + 'i', r) for r in new)
        head = bytearray(data[start:vstart])
        struct.pack_into(e + 'i', head, 16, len(body))
        return data[:start] + bytes(head) + body + data[vstart + size:]
    body = struct.pack(e + 'i', len(refs)) + b''.join(struct.pack(e + 'i', r) for r in refs)
    tag = struct.pack(e + 'iiiiii', pk.names.index('Includes'), 0, pk.names.index('ArrayProperty'), 0,
                      len(body), 0)
    return data[:none_at] + tag + body + data[none_at:]


def set_includes(pk, data, refs):
    """data with its Includes array replaced by refs (created if missing)"""
    e = pk.e
    props, none_at = tags(pk, data, 4)
    body = struct.pack(e + 'i', len(refs)) + b''.join(struct.pack(e + 'i', r) for r in refs)
    inc = [p for p in props if p[0] == 'Includes']
    if inc:
        _, _, start, vstart, size = inc[0]
        head = bytearray(data[start:vstart])
        struct.pack_into(e + 'i', head, 16, len(body))
        return data[:start] + bytes(head) + body + data[vstart + size:]
    tag = struct.pack(e + 'iiiiii', pk.names.index('Includes'), 0, pk.names.index('ArrayProperty'), 0,
                      len(body), 0)
    return data[:none_at] + tag + body + data[none_at:]


def includes_of(pk, data):
    e = pk.e
    props, _ = tags(pk, data, 4)
    for name, typ, start, vstart, size in props:
        if name == 'Includes':
            n = struct.unpack_from(e + 'i', data, vstart)[0]
            return [struct.unpack_from(e + 'i', data, vstart + 4 + 4 * k)[0] for k in range(n)]
    return []


def lzo_compress(chunk):
    lib = ctypes.CDLL('liblzo2.so.2')
    lib.__lzo_init_v2(1, -1, -1, -1, -1, -1, -1, -1, -1, -1)
    out = ctypes.create_string_buffer(len(chunk) + len(chunk) // 16 + 64 + 3)
    n = ctypes.c_ulong(0)
    wrk = ctypes.create_string_buffer(1 << 20)
    if lib.lzo1x_1_compress(chunk, ctypes.c_ulong(len(chunk)), out, ctypes.byref(n), wrk) != 0:
        raise ValueError('LZO compression failed')
    return out.raw[:n.value]


def compress_package(u):
    blocks = [u[i:i + BLOCK] for i in range(0, len(u), BLOCK)]
    comp = [lzo_compress(b) for b in blocks]
    head = struct.pack('<IIII', 0x9e2a83c1, BLOCK, sum(len(c) for c in comp), len(u))
    head += b''.join(struct.pack('<II', len(c), len(b)) for c, b in zip(comp, blocks))
    return head + b''.join(comp)


def patch(u):
    pk = upkparse.Package(u)
    index = {pk.obj_name(i + 1): i + 1 for i in range(len(pk.exports))}
    out = bytearray(u)
    log = []
    for pc_name, xbox_names in MERGES:
        pc = index.get('PCControllerConfigs.' + pc_name)
        refs = [index.get('ControllerConfigs.' + x) for x in xbox_names]
        if not pc or not all(refs):
            log.append('skipped %s (not found)' % pc_name)
            continue
        # the Xbox objects get a name of their own (number suffix 100: "UI0_99"): the game finds
        # configs, and their Includes, by short name, which found the PC object again [verified:
        # with the plain names the merged configs did not react to the pad]
        for r in refs:
            xe = pk.exports[r - 1]
            name_pos = xe.size_pos - 20                 # (name index, number before archetype, flags)
            struct.pack_into(pk.e + 'i', out, name_pos + 4, RENAME)
        ex = pk.exports[pc - 1]
        new = add_includes(pk, pk.export_data(ex), refs)
        if new == pk.export_data(ex):
            continue
        while len(out) % 4:
            out += b'\0'
        offset = len(out)
        out += new
        struct.pack_into(pk.e + 'ii', out, ex.size_pos, len(new), offset)
        log.append('PCControllerConfigs.%s + %s' % (pc_name, ', '.join('ControllerConfigs.' + x for x in xbox_names)))
    # the 360's alternate layouts (XBoxConfigAlt1-3: the standard layout with XBoxActionsAltN in
    # place of XBoxActions [verified: work/ctrl_pc]) made complete for PC: the patched PCConfig's
    # includes with that one set swapped, under their own names ("XBoxConfigAlt1_99"), pushed by
    # runtime/game.c in place of PCConfig when that layout is chosen
    pc = index['PCControllerConfigs.PCConfig']
    pkd = upkparse.Package(bytes(out))           # (PCConfig as just patched)
    pc_inc = includes_of(pkd, pkd.export_data(pkd.exports[pc - 1]))
    std = index['ControllerConfigs.XBoxActions']
    for n in (1, 2, 3):
        alt = index['ControllerConfigs.XBoxConfigAlt%d' % n]
        acts = index['ControllerConfigs.XBoxActionsAlt%d' % n]
        refs = [acts if r == std else r for r in pc_inc]
        xe = pk.exports[alt - 1]
        struct.pack_into(pk.e + 'i', out, xe.size_pos - 20 + 4, RENAME)
        new = set_includes(pk, pk.export_data(xe), refs)
        while len(out) % 4:
            out += b'\0'
        offset = len(out)
        out += new
        struct.pack_into(pk.e + 'ii', out, xe.size_pos, len(new), offset)
        log.append('ControllerConfigs.XBoxConfigAlt%d_99 = PCConfig with XBoxActionsAlt%d' % (n, n))
    return bytes(out), log


def patch_config(game, user, start_map=None):
    import coalesced
    src = os.path.join(game, 'StrangleholdGame', 'Config', 'PC', 'Cooked', 'Coalesced.ini')
    entries = []
    changed = 0
    for name, text in coalesced.read(src):
        base = name.replace('\\', '/').split('/')[-1]
        if base in ('PC-StrangleholdGame.ini', 'StrangleholdGame.ini'):
            old = '; ForceFeedbackManagerClassName=WinDrv.XnaForceFeedbackManager'
            if old in text:
                text = text.replace(old, 'ForceFeedbackManagerClassName=WinDrv.XnaForceFeedbackManager', 1)
                changed += 1
        if base in ('PC-StrangleholdInput.ini', 'StrangleholdInput.ini') and 'XBoxConfigAlt1_99' not in text:
            text = text.replace('AllControlConfigNames=PCControllerConfigs.PCConfig\r\n',
                                'AllControlConfigNames=PCControllerConfigs.PCConfig\r\n' +
                                ''.join('AllControlConfigNames=ControllerConfigs.XBoxConfigAlt%d_99\r\n' % n for n in (1, 2, 3)), 1)
        if start_map and base in ('PC-StrangleholdEngine.ini', 'StrangleholdEngine.ini'):
            text = text.replace('Map=intro_global_persistent', 'Map=' + start_map, 1)
        entries.append((name, text))
    dst = os.path.join(user, 'StrangleholdGame', 'Config', 'PC', 'Cooked')
    os.makedirs(dst, exist_ok=True)
    open(os.path.join(dst, 'Coalesced.ini'), 'wb').write(coalesced.pack(entries))
    print('Coalesced.ini: force feedback manager on (%d files)%s' % (changed, ', start map ' + start_map if start_map else ''))


def main():
    game, user = sys.argv[1:3]
    start_map = sys.argv[sys.argv.index('--map') + 1] if '--map' in sys.argv else None
    patch_config(game, user, start_map)
    src_dir = os.path.join(game, 'StrangleholdGame', 'CookedPC')
    dst_dir = os.path.join(user, 'StrangleholdGame', 'CookedPC')
    os.makedirs(dst_dir, exist_ok=True)
    for fn in sorted(os.listdir(src_dir)):
        if not (fn.startswith('Startup_') and fn.endswith('.xxx')):
            continue
        u = upkdecomp.decompress(open(os.path.join(src_dir, fn), 'rb').read())
        p, log = patch(u)
        check = upkparse.Package(p)          # (the tables still parse)
        assert check.export_count == upkparse.Package(u).export_count
        open(os.path.join(dst_dir, fn), 'wb').write(compress_package(p))
        with open(os.path.join(dst_dir, fn + '.uncompressed_size'), 'w') as fh:
            fh.write('%d' % len(p))
        print('%s: %d objects changed, %d -> %d bytes' % (fn, len([x for x in log if '+' in x or '=' in x]), len(u), len(p)))
        for line in log:
            if 'skipped' in line:
                print('  ' + line)


if __name__ == '__main__':
    main()
