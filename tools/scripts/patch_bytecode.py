#!/usr/bin/env python3
"""PS1 edits to Sonic 2's script bytecode, applied to the disc tree at build time (docs/30; the model is the Sonic
CD port's tool). The engine runs the scripts unchanged; only their data changes here: hot script loops become one
PS1 engine opcode (RSDKv4/Script.cpp, `#if RETRO_PLATFORM == RETRO_PS1` FunctionInfo list) that runs the same loop
natively, leaving the same temps and table / local writes behind.

- Oscillators (GlobalCode, the function Stage Setup calls every frame; ~390 VM instructions, ~90 hblanks a frame
  on the emulator): its body -> PS1Oscillate state, params, bits, count / return.
- Emerald Hill's Bridge (stage object "Bridge": its draw sub, ~170 VM instructions a frame) -> PS1BridgeDraw / End
  in every stage file whose Bridge draw sub is exactly BRIDGE_SIG (Hidden Palace's calls a script function per
  log: not patched).
- Lose Ring (GlobalCode object "Lose Ring": its update sub, the rings scattered on a hit) -> PS1LoseRing / End
  when it is exactly LOSERING_SIG.
- Metropolis's Horizontal Door (stage object: its update sub) -> PS1HorizontalDoor / End when exactly HDOOR_SIG.
- Metropolis's Button Bridge (stage object: its update sub, two switches) -> PS1ButtonBridge / End when the sub and
  its jump-table entries are exactly BUTTONBRIDGE_SIG.
- Plane switches (GlobalCode objects "Plane Sw V" / "Plane Sw H": their update subs) -> PS1PlaneSwitchV / H / End
  when exactly PLANESWV_SIG / PLANESWH_SIG.
- Ring (GlobalCode object "Ring": its update sub, ~21 VM instructions a frame per ring) -> PS1Ring / End, only
  when the sub is exactly RING_SIG (the instructions RSDKv4/Script.cpp PS1RingUpdate transcribes, constants
  included).

Every edit is found by its instruction pattern and checked (count and operands); anything unexpected stops the
build. Already patched files are recognised and left alone.

Usage: patch_bytecode.py BYTECODE_DIR   (the disc tree's Bytecode/, e.g. build/iso/Bytecode)
"""
import os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..'))
import bytecode_scan as bs  # noqa: E402


def encode(values):
    """RSDKv4 bytecode block encoding (LoadBytecode): u32 count, then blocks: header h, n = h & 0x7F values,
    4 bytes each if h >= 0x80 else 1 byte each (0..255)."""
    out = bytearray(struct.pack('<I', len(values)))
    i = 0
    while i < len(values):
        small = 0 <= values[i] <= 255
        j = i
        while j < len(values) and j - i < 127 and (0 <= values[j] <= 255) == small:
            j += 1
        out.append(j - i if small else 0x80 | (j - i))
        for v in values[i:j]:
            out += bytes([v]) if small else struct.pack('<i', v)
        i = j
    return bytes(out)


def instructions(code, start, names, vars_):
    """(position, name, operands) from `start` to the function's end; operand = (kind, value, position of the
    value): kind 'var' (value = (name, array info or None)), 'int' or 'str'."""
    out, p, depth = [], start, 0
    while p < len(code):
        at, op = p, code[p]
        p += 1
        name, size = names[op]
        ops = []
        for _ in range(size):
            t = code[p]
            p += 1
            if t == 1:
                arr = code[p]
                p += 1
                idx = None
                if arr in (1, 2, 3):
                    idx = (arr, code[p], code[p + 1])  # (array kind, 1 = array position register else constant, value)
                    p += 2
                ops.append(('var', (vars_[code[p]].replace('VAR_', ''), idx), p))
                p += 1
            elif t == 2:
                ops.append(('int', code[p], p))
                p += 1
            else:
                p += code[p] // 4 + 2
                ops.append(('str', None, p))
        out.append((at, name, ops))
        depth += (name in bs.OPEN) - (name in bs.CLOSE)
        if name in ('End', 'EndFunction') or (name == 'return' and depth <= 0):
            break
    return out


def T(n):
    return ('var', ('TEMP%d' % n, None))


def osc_pattern(c):
    """GlobalCode's oscillator function with its constants (state / params tables, the bits local, the count)."""
    L = ('var', ('LOCAL', (1, 0, c['bits'])))
    return [
        ('Equal', [T(0), ('int', 0)]),
        ('Equal', [T(1), ('int', 0)]),
        ('WLower', [('int', 0), T(0), ('int', c['n'])]),
        ('GetTableValue', [T(4), T(1), ('int', c['params'])]),
        ('GetTableValue', [T(6), T(1), ('int', c['state'])]),
        ('Inc', [T(1)]),
        ('GetTableValue', [T(5), T(1), ('int', c['params'])]),
        ('GetTableValue', [T(7), T(1), ('int', c['state'])]),
        ('Dec', [T(1)]),
        ('GetBit', [T(2), L, T(0)]),
        ('IfEqual', [('int', 2), T(2), ('int', 0)]),
        ('Add', [T(7), T(4)]),
        ('Add', [T(6), T(7)]),
        ('IfGreaterOrEqual', [('int', 4), T(6), T(5)]),
        ('SetBit', [L, T(0), ('int', 1)]),
        ('endif', []),
        ('else', []),
        ('Sub', [T(7), T(4)]),
        ('Add', [T(6), T(7)]),
        ('IfLower', [('int', 6), T(6), T(5)]),
        ('SetBit', [L, T(0), ('int', 0)]),
        ('endif', []),
        ('endif', []),
        ('SetTableValue', [T(6), T(1), ('int', c['state'])]),
        ('Inc', [T(1)]),
        ('SetTableValue', [T(7), T(1), ('int', c['state'])]),
        ('Inc', [T(1)]),
        ('Inc', [T(0)]),
        ('loop', []),
        ('return', []),
    ]


def listing(ins):
    """One line per instruction: name, then operands (variables with their array info)."""
    out = []
    for _, n, ops in ins:
        parts = []
        for o in ops:
            if o[0] == 'var':
                nm, idx = o[1]
                parts.append(nm if idx is None else '%s[%d,%d,%d]' % ((nm,) + tuple(idx)))
            else:
                parts.append(str(o[1]))
        out.append((n + ' ' + ' '.join(parts)).strip())
    return out


JUMP_OPS = ('IfEqual', 'IfGreater', 'IfGreaterOrEqual', 'IfLower', 'IfLowerOrEqual', 'IfNotEqual', 'WEqual', 'WGreater',
            'WGreaterOrEqual', 'WLower', 'WLowerOrEqual', 'WNotEqual', 'switch', 'ForEachActive', 'ForEachAll')


def jump_listing(ins, jt, start, jstart):
    """The jump-table entries a sub's jumps use (If / While: else / end; switch: low, high, default, end, cases;
    ForEach: its two), code offsets as instruction indices ('@k'), as extra listing lines."""
    idx = {at - start: k for k, (at, _, _) in enumerate(ins)}
    out = []
    for k, (_, n, ops) in enumerate(ins):
        if n not in JUMP_OPS:
            continue
        j = jstart + ops[0][1]
        if n == 'switch':
            lo, hi = jt[j], jt[j + 1]
            ents = ['%d' % lo, '%d' % hi] + ['@%s' % idx.get(o, '?%d' % o) for o in jt[j + 2:j + 4] + jt[j + 4:j + 5 + hi - lo]]
        else:
            ents = ['@%s' % idx.get(o, '?%d' % o) for o in jt[j:j + 2]]
        out.append('JT %d %s %s' % (k, n, ' '.join(ents)))
    return out


def sub_listing(path, obj, sub, names, vars_):
    """(full code, own code, base, pointer, listing with jump-table lines) of object `obj`'s sub in a stage file."""
    code, jt, subs, jsubs, _, _ = bs.full_code(path)
    own = bs.load(path)
    base = len(code) - len(own[0])
    jbase = 0
    if base:
        jbase = len(bs.load(os.path.join(os.path.dirname(path), 'GlobalCode.bin'))[1])
        jt = bs.load(os.path.join(os.path.dirname(path), 'GlobalCode.bin'))[1] + jt
    ptr, jptr = subs[obj][sub], jsubs[obj][sub]
    ins = instructions(code, ptr, names, vars_)
    return code, base, ptr, listing(ins) + jump_listing(ins, jt, ptr, jptr)


RING_SIG = [
    'ForEachActive 0 256 ARRAYPOS6',
    'CheckEqual OBJECTSTATE[1,0,0] 27',
    'Equal TEMP0 CHECKRESULT',
    'CheckEqual OBJECTSTATE[1,1,6] 27',
    'Equal TEMP0 CHECKRESULT',
    'CheckEqual OBJECTSTATE[1,1,6] 26',
    'Or TEMP0 CHECKRESULT',
    'IfEqual 2 TEMP0 0',
    'BoxCollisionTest 0 OBJECTENTITYPOS -8 -8 8 8 ARRAYPOS6 65536 65536 65536 65536',
    'IfEqual 4 CHECKRESULT 1',
    'Equal OBJECTTYPE 12',
    'Equal OBJECTDRAWORDER OBJECTVALUE18[1,1,6]',
    'IfEqual 6 OBJECTVALUE16[1,1,6] 1',
    'Inc OBJECTVALUE0[1,0,0]',
    'IfGreater 8 OBJECTVALUE0[1,0,0] 999',
    'Equal OBJECTVALUE0[1,0,0] 999',
    'endif',
    'else',
    'Inc OBJECTVALUE0[1,1,6]',
    'IfGreater 10 OBJECTVALUE0[1,1,6] 999',
    'Equal OBJECTVALUE0[1,1,6] 999',
    'endif',
    'endif',
    'IfGreaterOrEqual 12 OBJECTVALUE0[1,0,0] GLOBAL[1,0,21]',
    'IfNotEqual 14 GLOBAL[1,0,0] 2',
    'Inc GLOBAL[1,0,25]',
    'PlaySfx 24 0',
    'PauseMusic',
    'ResetObjectEntity 25 34 2 0 0',
    'Equal OBJECTPRIORITY[1,0,25] 1',
    'endif',
    'Add GLOBAL[1,0,21] 100',
    'IfGreater 16 GLOBAL[1,0,21] 300',
    'Equal GLOBAL[1,0,21] 1000',
    'endif',
    'endif',
    'IfEqual 18 GLOBAL[1,0,13] 1',
    'IfEqual 20 GLOBAL[1,0,108] 0',
    'IfEqual 22 ARRAYPOS6 0',
    'Inc GLOBAL[1,0,119]',
    'else',
    'Inc GLOBAL[1,0,125]',
    'endif',
    'else',
    'IfEqual 24 ARRAYPOS6 1',
    'Inc GLOBAL[1,0,119]',
    'else',
    'Inc GLOBAL[1,0,125]',
    'endif',
    'endif',
    'endif',
    'IfEqual 26 GLOBAL[1,0,20] 0',
    'PlaySfx 1 0',
    'SetSfxAttributes 1 -1 -100',
    'Equal GLOBAL[1,0,20] 1',
    'else',
    'PlaySfx 2 0',
    'SetSfxAttributes 2 -1 100',
    'Equal GLOBAL[1,0,20] 0',
    'endif',
    'else',
    'IfEqual 28 OBJECTSTATE 0',
    'IfEqual 30 OBJECTVALUE37[1,1,6] 4',
    'BoxCollisionTest 0 OBJECTENTITYPOS -64 -64 64 64 ARRAYPOS6 65536 65536 65536 65536',
    'IfEqual 32 CHECKRESULT 1',
    'Equal OBJECTSTATE 1',
    'Equal OBJECTVALUE1 ARRAYPOS6',
    'endif',
    'endif',
    'endif',
    'endif',
    'endif',
    'next',
    'IfEqual 34 OBJECTSTATE 1',
    'Equal ARRAYPOS0 OBJECTVALUE1',
    'IfNotEqual 36 OBJECTVALUE37[1,1,0] 4',
    'Equal OBJECTTYPE 11',
    'Equal OBJECTANIMATIONSPEED 128',
    'Equal OBJECTALPHA 256',
    'else',
    'Equal ARRAYPOS0 OBJECTVALUE1',
    'IfGreater 38 OBJECTXPOS OBJECTXPOS[1,1,0]',
    'IfGreater 40 OBJECTXVEL 0',
    'Sub OBJECTXVEL 49152',
    'else',
    'Sub OBJECTXVEL 12288',
    'endif',
    'else',
    'IfLower 42 OBJECTXVEL 0',
    'Add OBJECTXVEL 49152',
    'else',
    'Add OBJECTXVEL 12288',
    'endif',
    'endif',
    'IfGreater 44 OBJECTYPOS OBJECTYPOS[1,1,0]',
    'IfGreater 46 OBJECTYVEL 0',
    'Sub OBJECTYVEL 49152',
    'else',
    'Sub OBJECTYVEL 12288',
    'endif',
    'else',
    'IfLower 48 OBJECTYVEL 0',
    'Add OBJECTYVEL 49152',
    'else',
    'Add OBJECTYVEL 12288',
    'endif',
    'endif',
    'Add OBJECTXPOS OBJECTXVEL',
    'Add OBJECTYPOS OBJECTYVEL',
    'endif',
    'endif',
    'End',
]


def global_objects(bcdir):
    """GlobalCode's object names (GameConfig.bin, beside Bytecode/ in Data/Game)."""
    sys.path.insert(0, os.path.join(HERE, '..', 'atlas'))
    import object_sheets
    return object_sheets.game_config(os.path.join(bcdir, '..', 'Data'))[0]


BRIDGE_SIG = [
    'Equal TEMP0 0',
    'Equal TEMP1 OBJECTVALUE6',
    'Add TEMP1 524288',
    'Equal TEMP4 524288',
    'Equal TEMP5 OBJECTVALUE2',
    'ShR TEMP5 20',
    'WLower 0 TEMP0 TEMP5',
    'Equal TEMP3 TEMP4',
    'ShL TEMP3 7',
    'Div TEMP3 OBJECTVALUE2',
    'Sin TEMP2 TEMP3',
    'Mul TEMP2 OBJECTVALUE4',
    'ShR TEMP2 9',
    'Add TEMP2 OBJECTYPOS',
    'DrawSpriteXY 0 TEMP1 TEMP2',
    'Add TEMP1 1048576',
    'Add TEMP4 1048576',
    'Inc TEMP0',
    'loop',
    'Equal TEMP2 OBJECTVALUE4',
    'Add TEMP2 OBJECTYPOS',
    'DrawSpriteXY 0 TEMP1 TEMP2',
    'Add TEMP1 1048576',
    'Inc TEMP0',
    'Equal TEMP5 OBJECTVALUE7',
    'Sub TEMP5 OBJECTVALUE6',
    'Sub TEMP5 OBJECTVALUE2',
    'Equal TEMP1 OBJECTVALUE7',
    'Sub TEMP1 524288',
    'Equal TEMP4 524288',
    'WLower 2 TEMP0 OBJECTPROPERTYVALUE',
    'Equal TEMP3 TEMP4',
    'ShL TEMP3 7',
    'Div TEMP3 TEMP5',
    'Sin TEMP2 TEMP3',
    'Mul TEMP2 OBJECTVALUE4',
    'ShR TEMP2 9',
    'Add TEMP2 OBJECTYPOS',
    'DrawSpriteXY 0 TEMP1 TEMP2',
    'Sub TEMP1 1048576',
    'Add TEMP4 1048576',
    'Inc TEMP0',
    'loop',
    'End',
]


def patch_stage_sub(bcdir, obj_name, sub, sig, op_name, with_jumps=False, funcs=None, calls=()):
    """Every stage file whose object `obj_name` has sub `sub` (0 update, 1 draw) equal to `sig` -> op / End
    (with_jumps: the signature also holds the sub's jump-table entries, jump_listing; funcs: {function id:
    signature} of the script functions the sub calls that the native inlines, checked in the same file; calls: the
    script functions the native runs in the VM, callable_from_native)."""
    names, vars_ = bs.tables(3)
    fn = [n for n, _ in names]
    op, end = fn.index(op_name), fn.index('End')
    sys.path.insert(0, os.path.join(HERE, '..', 'atlas'))
    import object_sheets
    data = os.path.join(bcdir, '..', 'Data')
    glen = len(bs.load(os.path.join(bcdir, 'GlobalCode.bin'))[0])
    done, already = [], []
    for f in sorted(os.listdir(bcdir)):
        folder = f[:-4]
        if not f.endswith('.bin') or folder == 'GlobalCode':
            continue
        cfg = os.path.join(data, 'Stages', folder, 'StageConfig.bin')
        if not os.path.exists(cfg):
            continue
        load_globals, _, objs = object_sheets.stage_config(data, folder)
        if obj_name not in objs:
            continue
        path = os.path.join(bcdir, f)
        raw = open(path, 'rb').read()
        own, p = bs.blocks(raw, 0)
        rest = raw[p:]
        full = bs.full_code(path)[0]
        ptr = bs.load(path)[2][objs.index(obj_name)][sub]
        base = glen if len(full) > len(own) else 0
        if full[ptr] == op:
            already.append(folder)
            continue
        got = sub_listing(path, objs.index(obj_name), sub, names, vars_)[3] if with_jumps else \
            listing(instructions(full, ptr, names, vars_))
        if got != sig:
            continue
        fns = bs.load(path)[4]  # a stage file's table holds every function (LoadBytecode fills the list from 0)
        if any(listing(instructions(full, fns[f][0], names, vars_)) != fsig for f, fsig in (funcs or {}).items()):
            continue
        if calls and callable_from_native(path, calls):
            continue
        own[ptr - base:ptr - base + 2] = [op, end]
        open(path, 'wb').write(encode(own) + rest)
        assert bs.blocks(open(path, 'rb').read(), 0)[0] == own
        done.append(folder)
    return '%s native in %s%s' % (op_name, ' '.join(done) or '-', ' (already: %s)' % ' '.join(already) if already else '')


LOSERING_SIG = [
    'Add OBJECTYVEL 6144',
    'Add OBJECTXPOS OBJECTXVEL',
    'Add OBJECTYPOS OBJECTYVEL',
    'IfGreaterOrEqual 0 OBJECTYVEL 0',
    'ObjectTileCollision 0 0 8 0',
    'IfEqual 2 CHECKRESULT 1',
    'Equal TEMP0 OBJECTYVEL',
    'ShR TEMP0 2',
    'Sub OBJECTYVEL TEMP0',
    'FlipSign OBJECTYVEL',
    'IfGreater 4 OBJECTYVEL -65536',
    'Equal OBJECTYVEL -65536',
    'endif',
    'endif',
    'endif',
    'Inc OBJECTVALUE0',
    'IfEqual 6 OBJECTVALUE0 256',
    'Equal OBJECTTYPE 0',
    'Equal OBJECTXVEL 0',
    'Equal OBJECTYVEL 0',
    'Equal OBJECTVALUE0 0',
    'else',
    'Add OBJECTANIMATIONTIMER OBJECTANIMATIONSPEED',
    'IfGreater 8 OBJECTANIMATIONTIMER 255',
    'Sub OBJECTANIMATIONTIMER 256',
    'Inc OBJECTFRAME',
    'IfEqual 10 OBJECTFRAME 8',
    'Equal OBJECTFRAME 0',
    'IfGreater 12 OBJECTANIMATIONSPEED 16',
    'Sub OBJECTANIMATIONSPEED 16',
    'endif',
    'endif',
    'endif',
    'IfGreaterOrEqual 14 OBJECTVALUE0 240',
    'Sub OBJECTALPHA 16',
    'endif',
    'endif',
    'IfGreater 16 OBJECTVALUE0 63',
    'ForEachActive 18 256 ARRAYPOS6',
    'CheckEqual OBJECTSTATE[1,1,6] 27',
    'Equal TEMP0 CHECKRESULT',
    'CheckEqual OBJECTSTATE[1,1,6] 26',
    'Or TEMP0 CHECKRESULT',
    'IfEqual 20 TEMP0 0',
    'BoxCollisionTest 0 OBJECTENTITYPOS -8 -8 8 8 ARRAYPOS6 65536 65536 65536 65536',
    'IfEqual 22 CHECKRESULT 1',
    'Equal OBJECTTYPE 12',
    'IfEqual 24 OBJECTPROPERTYVALUE 0',
    'Equal OBJECTDRAWORDER 4',
    'else',
    'Equal OBJECTDRAWORDER 2',
    'endif',
    'Equal OBJECTVALUE0 0',
    'Equal OBJECTVALUE1 0',
    'Equal OBJECTVALUE0 0',
    'Equal OBJECTFRAME 0',
    'IfEqual 26 OBJECTVALUE16[1,1,6] 1',
    'Inc OBJECTVALUE0[1,0,0]',
    'IfGreater 28 OBJECTVALUE0[1,0,0] 999',
    'Equal OBJECTVALUE0[1,0,0] 999',
    'endif',
    'else',
    'Inc OBJECTVALUE0[1,1,6]',
    'IfGreater 30 OBJECTVALUE0[1,1,6] 999',
    'Equal OBJECTVALUE0[1,1,6] 999',
    'endif',
    'endif',
    'IfGreaterOrEqual 32 OBJECTVALUE0[1,0,0] GLOBAL[1,0,21]',
    'IfNotEqual 34 GLOBAL[1,0,0] 2',
    'Inc GLOBAL[1,0,25]',
    'PlaySfx 24 0',
    'PauseMusic',
    'ResetObjectEntity 25 34 2 0 0',
    'Equal OBJECTPRIORITY[1,0,25] 1',
    'Add GLOBAL[1,0,21] 100',
    'IfGreater 36 GLOBAL[1,0,21] 300',
    'Equal GLOBAL[1,0,21] 1000',
    'endif',
    'endif',
    'endif',
    'IfEqual 38 GLOBAL[1,0,20] 0',
    'PlaySfx 1 0',
    'SetSfxAttributes 1 -1 -100',
    'Equal GLOBAL[1,0,20] 1',
    'else',
    'PlaySfx 2 0',
    'SetSfxAttributes 2 -1 100',
    'Equal GLOBAL[1,0,20] 0',
    'endif',
    'endif',
    'endif',
    'next',
    'endif',
    'End',
]


HDOOR_SIG = [
    'Equal TEMP0 0',
    'Equal TEMP1 OBJECTXPOS',
    'Equal OBJECTXPOS OBJECTVALUE1',
    'ForEachActive 0 256 ARRAYPOS6',
    'IfEqual 2 OBJECTDIRECTION 0',
    'BoxCollisionTest 0 OBJECTENTITYPOS -32 -16 96 64 ARRAYPOS6 -1 -1 1 1',
    'IfEqual 4 CHECKRESULT 1',
    'Equal TEMP0 1',
    'endif',
    'else',
    'BoxCollisionTest 0 OBJECTENTITYPOS -160 -16 -32 64 ARRAYPOS6 -1 -1 1 1',
    'IfEqual 6 CHECKRESULT 1',
    'Equal TEMP0 1',
    'endif',
    'endif',
    'next',
    'Equal OBJECTXPOS TEMP1',
    'IfEqual 8 TEMP0 1',
    'IfLower 10 OBJECTVALUE0 4',
    'Inc OBJECTVALUE0',
    'IfEqual 12 OBJECTDIRECTION 0',
    'Sub OBJECTXPOS 1048576',
    'else',
    'Add OBJECTXPOS 1048576',
    'endif',
    'endif',
    'else',
    'IfGreater 14 OBJECTVALUE0 0',
    'Dec OBJECTVALUE0',
    'IfEqual 16 OBJECTDIRECTION 0',
    'Add OBJECTXPOS 1048576',
    'else',
    'Sub OBJECTXPOS 1048576',
    'endif',
    'endif',
    'endif',
    'ForEachActive 18 256 ARRAYPOS6',
    'IfGreaterOrEqual 20 OBJECTYVEL[1,1,6] 0',
    'BoxCollisionTest 1 OBJECTENTITYPOS -32 -12 32 12 ARRAYPOS6 65536 65536 65536 65536',
    'endif',
    'next',
    'End',
]


BUTTONBRIDGE_SIG = [
    'switch 0 OBJECTSTATE',
    'IfEqual 8 OBJECTVALUE0[2,0,1] 1',
    'IfEqual 10 OBJECTPROPERTYVALUE 0',
    'Equal OBJECTSTATE 1',
    'else',
    'Equal OBJECTSTATE 3',
    'endif',
    'endif',
    'ForEachActive 12 256 ARRAYPOS6',
    'BoxCollisionTest 1 OBJECTENTITYPOS -64 -12 64 8 ARRAYPOS6 65536 65536 65536 65536',
    'next',
    'break',
    'Equal TEMP0 OBJECTXPOS',
    'And TEMP0 -65536',
    'IfEqual 14 OBJECTDIRECTION 0',
    'Sub OBJECTXPOS 131072',
    'Inc OBJECTFRAME',
    'else',
    'Add OBJECTXPOS 131072',
    'Dec OBJECTFRAME',
    'endif',
    'And OBJECTFRAME 3',
    'Dec OBJECTVALUE0',
    'IfLowerOrEqual 16 OBJECTVALUE0 0',
    'IfEqual 18 OBJECTPROPERTYVALUE 0',
    'Equal OBJECTSTATE 2',
    'else',
    'Equal OBJECTSTATE 0',
    'Equal OBJECTVALUE0 64',
    'endif',
    'endif',
    'Equal TEMP1 OBJECTXPOS',
    'And TEMP1 -65536',
    'Sub TEMP1 TEMP0',
    'Equal TEMP2 OBJECTXPOS',
    'Equal OBJECTXPOS TEMP0',
    'ForEachActive 20 256 ARRAYPOS6',
    'BoxCollisionTest 1 OBJECTENTITYPOS -64 -12 64 8 ARRAYPOS6 65536 65536 65536 65536',
    'switch 22 CHECKRESULT',
    'Add OBJECTXPOS[1,1,6] TEMP1',
    'break',
    'IfEqual 29 OBJECTSTATE[1,1,6] 24',
    'Equal OBJECTVALUE1[1,1,6] 0',
    'Equal OBJECTANIMATION[1,1,6] GLOBAL[1,0,86]',
    'Equal OBJECTSTATE[1,1,6] 22',
    'endif',
    'break',
    'endswitch',
    'next',
    'Equal OBJECTXPOS TEMP2',
    'break',
    'Inc OBJECTVALUE0',
    'IfEqual 31 OBJECTVALUE0 180',
    'Equal OBJECTVALUE0 0',
    'IfEqual 33 OBJECTPROPERTYVALUE 0',
    'Equal OBJECTSTATE 3',
    'else',
    'Equal OBJECTSTATE 1',
    'endif',
    'Equal OBJECTVALUE0 64',
    'endif',
    'ForEachActive 35 256 ARRAYPOS6',
    'BoxCollisionTest 1 OBJECTENTITYPOS -64 -12 64 8 ARRAYPOS6 65536 65536 65536 65536',
    'next',
    'break',
    'Equal TEMP0 OBJECTXPOS',
    'And TEMP0 -65536',
    'IfEqual 37 OBJECTDIRECTION 0',
    'Add OBJECTXPOS 131072',
    'Dec OBJECTFRAME',
    'else',
    'Sub OBJECTXPOS 131072',
    'Inc OBJECTFRAME',
    'endif',
    'And OBJECTFRAME 3',
    'Dec OBJECTVALUE0',
    'IfLowerOrEqual 39 OBJECTVALUE0 0',
    'IfEqual 41 OBJECTPROPERTYVALUE 0',
    'Equal OBJECTSTATE 0',
    'Equal OBJECTVALUE0 64',
    'else',
    'Equal OBJECTSTATE 2',
    'endif',
    'endif',
    'Equal TEMP1 OBJECTXPOS',
    'And TEMP1 -65536',
    'Sub TEMP1 TEMP0',
    'Equal TEMP2 OBJECTXPOS',
    'Equal OBJECTXPOS TEMP0',
    'ForEachActive 43 256 ARRAYPOS6',
    'BoxCollisionTest 1 OBJECTENTITYPOS -64 -12 64 8 ARRAYPOS6 65536 65536 65536 65536',
    'switch 45 CHECKRESULT',
    'Add OBJECTXPOS[1,1,6] TEMP1',
    'break',
    'IfEqual 52 OBJECTSTATE[1,1,6] 24',
    'Equal OBJECTVALUE1[1,1,6] 0',
    'Equal OBJECTANIMATION[1,1,6] GLOBAL[1,0,86]',
    'Equal OBJECTSTATE[1,1,6] 22',
    'endif',
    'break',
    'endswitch',
    'next',
    'Equal OBJECTXPOS TEMP2',
    'break',
    'endswitch',
    'End',
    'JT 0 switch 0 3 @104 @105 @1 @12 @51 @65',
    'JT 1 IfEqual @7 @8',
    'JT 2 IfEqual @5 @7',
    'JT 8 ForEachActive @8 @11',
    'JT 14 IfEqual @18 @21',
    'JT 23 IfLowerOrEqual @30 @31',
    'JT 24 IfEqual @27 @30',
    'JT 36 ForEachActive @36 @49',
    'JT 38 switch 1 3 @47 @48 @39 @41 @41',
    'JT 41 IfEqual @45 @46',
    'JT 52 IfEqual @60 @61',
    'JT 54 IfEqual @57 @59',
    'JT 61 ForEachActive @61 @64',
    'JT 67 IfEqual @71 @74',
    'JT 76 IfLowerOrEqual @83 @84',
    'JT 77 IfEqual @81 @83',
    'JT 89 ForEachActive @89 @102',
    'JT 91 switch 1 3 @100 @101 @92 @94 @94',
    'JT 94 IfEqual @98 @99',
]


PLANESWV_SIG = [
    'ForEachActive 0 256 ARRAYPOS6',
    'CheckEqual OBJECTVALUE6 0',
    'Equal TEMP0 CHECKRESULT',
    'CheckEqual OBJECTGRAVITY[1,1,6] 0',
    'Or TEMP0 CHECKRESULT',
    'IfEqual 2 TEMP0 1',
    'BoxCollisionTest 0 OBJECTENTITYPOS -12 OBJECTVALUE0 12 OBJECTVALUE1 ARRAYPOS6 0 0 0 0',
    'IfEqual 4 CHECKRESULT 1',
    'IfGreater 6 OBJECTXVEL[1,1,6] 0',
    'GetBit TEMP0 OBJECTPROPERTYVALUE 6',
    'IfEqual 8 TEMP0 0',
    'Equal OBJECTCOLLISIONPLANE[1,1,6] OBJECTVALUE3',
    'endif',
    'Equal OBJECTVALUE18[1,1,6] OBJECTVALUE5',
    'else',
    'GetBit TEMP0 OBJECTPROPERTYVALUE 6',
    'IfEqual 10 TEMP0 0',
    'Equal OBJECTCOLLISIONPLANE[1,1,6] OBJECTVALUE2',
    'endif',
    'Equal OBJECTVALUE18[1,1,6] OBJECTVALUE4',
    'endif',
    'endif',
    'endif',
    'next',
    'End',
]

PLANESWH_SIG = [
    'ForEachActive 0 256 ARRAYPOS6',
    'CheckEqual OBJECTVALUE6 0',
    'Equal TEMP0 CHECKRESULT',
    'CheckEqual OBJECTGRAVITY[1,1,6] 0',
    'Or TEMP0 CHECKRESULT',
    'CheckEqual OBJECTSTATE[1,1,6] 24',
    'Or TEMP0 CHECKRESULT',
    'IfEqual 2 TEMP0 1',
    'BoxCollisionTest 0 OBJECTENTITYPOS OBJECTVALUE0 -12 OBJECTVALUE1 12 ARRAYPOS6 0 0 0 0',
    'IfEqual 4 CHECKRESULT 1',
    'IfGreater 6 OBJECTYVEL[1,1,6] 0',
    'GetBit TEMP0 OBJECTPROPERTYVALUE 6',
    'IfEqual 8 TEMP0 0',
    'Equal OBJECTCOLLISIONPLANE[1,1,6] OBJECTVALUE3',
    'endif',
    'Equal OBJECTVALUE18[1,1,6] OBJECTVALUE5',
    'else',
    'GetBit TEMP0 OBJECTPROPERTYVALUE 6',
    'IfEqual 10 TEMP0 0',
    'Equal OBJECTCOLLISIONPLANE[1,1,6] OBJECTVALUE2',
    'endif',
    'Equal OBJECTVALUE18[1,1,6] OBJECTVALUE4',
    'endif',
    'endif',
    'endif',
    'next',
    'End',
]


# Hidden Palace Rotate Platform: update sub (calls GlobalCode function 45, inlined by the native), draw sub.
ROTPLAT_SIG = [
    'IfEqual 0 OBJECTSTATE 1',
    'Equal TEMP0 GLOBAL[1,0,17]',
    'ShL TEMP0 1',
    'Sin OBJECTVALUE11 TEMP0',
    'ShR OBJECTVALUE11 3',
    'Add OBJECTVALUE11 192',
    'else',
    'Equal OBJECTVALUE11 256',
    'endif',
    'Equal TEMP2 OBJECTXPOS',
    'Equal TEMP3 OBJECTYPOS',
    'Add OBJECTVALUE0 OBJECTVALUE6',
    'And OBJECTVALUE0 131071',
    'Equal TEMP0 OBJECTVALUE0',
    'ShR TEMP0 8',
    'Equal TEMP1 OBJECTVALUE5',
    'Inc TEMP1',
    'ShL TEMP1 4',
    'Equal OBJECTXPOS OBJECTVALUE1',
    'Equal OBJECTYPOS OBJECTVALUE2',
    'Cos256 OBJECTVALUE3 TEMP0',
    'Mul OBJECTVALUE3 TEMP1',
    'Mul OBJECTVALUE3 OBJECTVALUE11',
    'Add OBJECTVALUE3 TEMP2',
    'And OBJECTVALUE3 -65536',
    'Equal OBJECTVALUE1 OBJECTVALUE3',
    'Sub OBJECTVALUE3 OBJECTXPOS',
    'Sin256 OBJECTVALUE4 TEMP0',
    'Mul OBJECTVALUE4 TEMP1',
    'Mul OBJECTVALUE4 OBJECTVALUE11',
    'Add OBJECTVALUE4 TEMP3',
    'And OBJECTVALUE4 -65536',
    'Equal OBJECTVALUE2 OBJECTVALUE4',
    'Sub OBJECTVALUE4 OBJECTYPOS',
    'IfEqual 2 OBJECTVALUE7 0',
    'ForEachActive 4 256 ARRAYPOS6',
    'BoxCollisionTest 3 OBJECTENTITYPOS -24 -8 24 8 ARRAYPOS6 65536 65536 65536 65536',
    'IfEqual 6 CHECKRESULT 1',
    'Add OBJECTXPOS[1,1,6] OBJECTVALUE3',
    'Add OBJECTYPOS[1,1,6] OBJECTVALUE4',
    'endif',
    'next',
    'else',
    'ForEachActive 8 256 ARRAYPOS6',
    'BoxCollisionTest 0 OBJECTENTITYPOS -16 -16 16 16 ARRAYPOS6 65536 65536 65536 65536',
    'IfEqual 10 CHECKRESULT 1',
    'CallFunction 45',
    'endif',
    'next',
    'endif',
    'Equal OBJECTXPOS TEMP2',
    'Equal OBJECTYPOS TEMP3',
    'Equal TEMP0 OBJECTVALUE9',
    'ShR TEMP0 2',
    'GetTableValue OBJECTVALUE10 TEMP0 84593',
    'Inc OBJECTVALUE9',
    'And OBJECTVALUE9 31',
    'End',
]


ROTPLAT_DRAW_SIG = [
    'Equal TEMP0 0',
    'Equal TEMP1 16',
    'Equal TEMP4 OBJECTVALUE0',
    'ShR TEMP4 8',
    'WLower 0 TEMP0 OBJECTVALUE5',
    'Cos256 TEMP2 TEMP4',
    'Mul TEMP2 TEMP1',
    'Mul TEMP2 OBJECTVALUE11',
    'Add TEMP2 OBJECTXPOS',
    'Sin256 TEMP3 TEMP4',
    'Mul TEMP3 TEMP1',
    'Mul TEMP3 OBJECTVALUE11',
    'Add TEMP3 OBJECTYPOS',
    'DrawSpriteXY 3 TEMP2 TEMP3',
    'Inc TEMP0',
    'Add TEMP1 16',
    'loop',
    'Cos256 OBJECTVALUE1 TEMP4',
    'Mul OBJECTVALUE1 TEMP1',
    'Mul OBJECTVALUE1 OBJECTVALUE11',
    'Add OBJECTVALUE1 OBJECTXPOS',
    'And OBJECTVALUE1 -65536',
    'Sin256 OBJECTVALUE2 TEMP4',
    'Mul OBJECTVALUE2 TEMP1',
    'Mul OBJECTVALUE2 OBJECTVALUE11',
    'Add OBJECTVALUE2 OBJECTYPOS',
    'And OBJECTVALUE2 -65536',
    'DrawSpriteXY OBJECTVALUE8 OBJECTVALUE1 OBJECTVALUE2',
    'DrawSprite OBJECTVALUE10',
    'End',
]


# GlobalCode function 45 (the player touching a platform is pushed off it), inlined by PS1RotatePlatform.
PUSHPLAYER_FN_SIG = [
    'IfNotEqual 0 OBJECTSTATE[1,1,6] 28',
    'Equal ARRAYPOS0 OBJECTENTITYPOS[1,1,6]',
    'Add ARRAYPOS0 ARRAYPOS7',
    'IfEqual 2 OBJECTVALUE7[1,1,6] 0',
    'IfEqual 4 OBJECTVALUE8[1,1,6] 0',
    'Equal OBJECTSTATE[1,1,6] 26',
    'IfGreater 6 OBJECTXPOS[1,1,6] OBJECTXPOS',
    'Equal OBJECTSPEED[1,1,6] 131072',
    'else',
    'Equal OBJECTSPEED[1,1,6] -131072',
    'endif',
    'endif',
    'endif',
    'endif',
    'return',
]


# Hidden Palace Bridge: update sub, draw sub (calls stage function 114 per log, inlined by the native).
HPZBRIDGE_SIG = [
    'IfGreater 0 OBJECTVALUE0 0',
    'IfLower 2 OBJECTVALUE1 128',
    'Add OBJECTVALUE1 8',
    'endif',
    'else',
    'IfGreater 4 OBJECTVALUE1 0',
    'Sub OBJECTVALUE1 8',
    'Equal OBJECTVALUE5 -1',
    'else',
    'Equal OBJECTVALUE3 0',
    'endif',
    'endif',
    'Equal OBJECTVALUE4 OBJECTVALUE3',
    'Mul OBJECTVALUE4 OBJECTVALUE1',
    'ShR OBJECTVALUE4 7',
    'Equal OBJECTVALUE0 0',
    'Equal OBJECTVALUE8 -5',
    'Equal OBJECTVALUE11 -5',
    'ForEachActive 6 256 ARRAYPOS6',
    'IfNotEqual 8 OBJECTSTATE[1,1,6] 25',
    'IfGreater 10 OBJECTXPOS[1,1,6] OBJECTVALUE6',
    'IfLower 12 OBJECTXPOS[1,1,6] OBJECTVALUE7',
    'IfEqual 14 ARRAYPOS6 OBJECTVALUE5',
    'Equal OBJECTVALUE2 OBJECTXPOS[1,1,6]',
    'Sub OBJECTVALUE2 OBJECTVALUE6',
    'Equal TEMP0 OBJECTVALUE2',
    'ShR TEMP0 8',
    'Equal TEMP1 OBJECTVALUE7',
    'Sub TEMP1 OBJECTVALUE6',
    'Equal TEMP2 TEMP1',
    'ShR TEMP2 16',
    'Div TEMP0 TEMP2',
    'Sin OBJECTVALUE3 TEMP0',
    'ShR TEMP1 13',
    'Mul OBJECTVALUE3 TEMP1',
    'Equal TEMP0 OBJECTYPOS',
    'Sub TEMP0 3145728',
    'IfGreater 16 OBJECTYPOS[1,1,6] TEMP0',
    'IfGreaterOrEqual 18 OBJECTYVEL[1,1,6] 0',
    'Equal TEMP2 OBJECTCOLLISIONBOTTOM[1,1,6]',
    'FlipSign TEMP2',
    'ShL TEMP2 16',
    'Add TEMP2 OBJECTVALUE4',
    'Sub TEMP2 524288',
    'Inc OBJECTVALUE0',
    'Equal OBJECTYPOS[1,1,6] OBJECTYPOS',
    'Add OBJECTYPOS[1,1,6] TEMP2',
    'Equal OBJECTGRAVITY[1,1,6] 0',
    'Equal OBJECTYVEL[1,1,6] 0',
    'Equal OBJECTFLOORSENSORL[1,1,6] 1',
    'Equal OBJECTFLOORSENSORC[1,1,6] 1',
    'Equal OBJECTFLOORSENSORR[1,1,6] 1',
    'Equal OBJECTVALUE8 OBJECTVALUE2',
    'ShR OBJECTVALUE8 20',
    'else',
    'Equal OBJECTVALUE5 -2',
    'endif',
    'endif',
    'else',
    'IfGreaterOrEqual 20 OBJECTYVEL[1,1,6] 0',
    'Equal TEMP0 OBJECTXPOS[1,1,6]',
    'Sub TEMP0 OBJECTVALUE6',
    'IfGreater 22 TEMP0 OBJECTVALUE2',
    'Equal TEMP0 OBJECTVALUE7',
    'Sub TEMP0 OBJECTXPOS[1,1,6]',
    'Equal TEMP3 OBJECTVALUE7',
    'Sub TEMP3 OBJECTVALUE6',
    'Sub TEMP3 OBJECTVALUE2',
    'Equal TEMP1 TEMP0',
    'ShL TEMP1 7',
    'Div TEMP1 TEMP3',
    'else',
    'Equal TEMP1 TEMP0',
    'ShL TEMP1 7',
    'Div TEMP1 OBJECTVALUE2',
    'endif',
    'Sin TEMP2 TEMP1',
    'Mul TEMP2 OBJECTVALUE4',
    'ShR TEMP2 9',
    'Sub TEMP2 524288',
    'IfLower 24 OBJECTYVEL[1,1,6] 32768',
    'Equal TEMP3 TEMP2',
    'ShR TEMP3 16',
    'Equal TEMP4 TEMP3',
    'Sub TEMP3 8',
    'else',
    'Equal TEMP3 TEMP2',
    'ShR TEMP3 16',
    'Equal TEMP4 TEMP3',
    'Add TEMP4 8',
    'endif',
    'BoxCollisionTest 0 OBJECTENTITYPOS -1024 TEMP3 1024 TEMP4 ARRAYPOS6 65536 65536 65536 65536',
    'IfEqual 26 CHECKRESULT 1',
    'Inc OBJECTVALUE0',
    'Equal OBJECTYPOS[1,1,6] OBJECTCOLLISIONBOTTOM[1,1,6]',
    'FlipSign OBJECTYPOS[1,1,6]',
    'ShL OBJECTYPOS[1,1,6] 16',
    'Add OBJECTYPOS[1,1,6] OBJECTYPOS',
    'Add OBJECTYPOS[1,1,6] TEMP2',
    'Equal OBJECTFLOORSENSORL[1,1,6] 1',
    'Equal OBJECTFLOORSENSORC[1,1,6] 1',
    'Equal OBJECTFLOORSENSORR[1,1,6] 1',
    'Equal OBJECTVALUE11 OBJECTXPOS[1,1,6]',
    'Sub OBJECTVALUE11 OBJECTVALUE6',
    'ShR OBJECTVALUE11 20',
    'IfEqual 28 ARRAYPOS6 0',
    'IfLower 30 OBJECTVALUE5 4',
    'IfGreater 32 OBJECTVALUE5 0',
    'Equal OBJECTVALUE2 OBJECTXPOS[1,1,6]',
    'Sub OBJECTVALUE2 OBJECTVALUE6',
    'Equal TEMP0 OBJECTVALUE2',
    'ShR TEMP0 8',
    'Equal TEMP1 OBJECTVALUE7',
    'Sub TEMP1 OBJECTVALUE6',
    'Equal TEMP2 TEMP1',
    'ShR TEMP2 16',
    'Div TEMP0 TEMP2',
    'Sin OBJECTVALUE3 TEMP0',
    'ShR TEMP1 13',
    'Mul OBJECTVALUE3 TEMP1',
    'Equal OBJECTVALUE4 OBJECTVALUE3',
    'Mul OBJECTVALUE4 OBJECTVALUE1',
    'ShR OBJECTVALUE4 7',
    'endif',
    'Equal OBJECTVALUE5 0',
    'IfLower 34 OBJECTYVEL[1,1,6] 256',
    'Equal OBJECTVALUE1 128',
    'endif',
    'endif',
    'else',
    'IfEqual 36 OBJECTVALUE5 -1',
    'Equal OBJECTVALUE5 ARRAYPOS6',
    'IfLower 38 OBJECTYVEL[1,1,6] 256',
    'Equal OBJECTVALUE1 128',
    'endif',
    'endif',
    'IfEqual 40 OBJECTVALUE5 -2',
    'Equal OBJECTVALUE5 ARRAYPOS6',
    'endif',
    'endif',
    'Equal OBJECTGRAVITY[1,1,6] 0',
    'Equal OBJECTYVEL[1,1,6] 0',
    'endif',
    'endif',
    'endif',
    'else',
    'IfEqual 42 ARRAYPOS6 OBJECTVALUE5',
    'Equal OBJECTVALUE5 -2',
    'Equal OBJECTVALUE1 32',
    'endif',
    'endif',
    'else',
    'IfEqual 44 ARRAYPOS6 OBJECTVALUE5',
    'Equal OBJECTVALUE5 -2',
    'Equal OBJECTVALUE1 32',
    'endif',
    'endif',
    'endif',
    'next',
    'Equal OBJECTVALUE9 OBJECTVALUE8',
    'Dec OBJECTVALUE9',
    'Equal OBJECTVALUE10 OBJECTVALUE8',
    'Inc OBJECTVALUE10',
    'Equal OBJECTVALUE12 OBJECTVALUE11',
    'Dec OBJECTVALUE12',
    'Equal OBJECTVALUE13 OBJECTVALUE11',
    'Inc OBJECTVALUE13',
    'IfNotEqual 46 OBJECTVALUE14 OBJECTVALUE8',
    'Equal OBJECTVALUE14 OBJECTVALUE8',
    'Equal OBJECTVALUE16 0',
    'endif',
    'IfNotEqual 48 OBJECTVALUE15 OBJECTVALUE11',
    'Equal OBJECTVALUE15 OBJECTVALUE11',
    'Equal OBJECTVALUE17 0',
    'endif',
    'Equal TEMP0 OBJECTVALUE16',
    'ShR TEMP0 2',
    'GetTableValue OBJECTVALUE18 TEMP0 73160',
    'GetTableValue OBJECTVALUE19 TEMP0 73169',
    'Inc OBJECTVALUE16',
    'And OBJECTVALUE16 31',
    'Equal TEMP0 OBJECTVALUE17',
    'ShR TEMP0 2',
    'GetTableValue OBJECTVALUE20 TEMP0 73160',
    'GetTableValue OBJECTVALUE21 TEMP0 73169',
    'Inc OBJECTVALUE17',
    'And OBJECTVALUE17 31',
    'End',
]


HPZBRIDGE_DRAW_SIG = [
    'Equal OBJECTVALUE22 0',
    'Equal TEMP0 0',
    'Equal TEMP1 OBJECTVALUE6',
    'Add TEMP1 524288',
    'Equal TEMP4 524288',
    'Equal TEMP5 OBJECTVALUE2',
    'ShR TEMP5 20',
    'WLower 0 TEMP0 TEMP5',
    'Equal TEMP3 TEMP4',
    'ShL TEMP3 7',
    'Div TEMP3 OBJECTVALUE2',
    'Sin TEMP2 TEMP3',
    'Mul TEMP2 OBJECTVALUE4',
    'ShR TEMP2 9',
    'Add TEMP2 OBJECTYPOS',
    'CallFunction 114',
    'Add TEMP1 1048576',
    'Add TEMP4 1048576',
    'Inc TEMP0',
    'Inc OBJECTVALUE22',
    'loop',
    'Equal TEMP2 OBJECTVALUE4',
    'Add TEMP2 OBJECTYPOS',
    'CallFunction 114',
    'Add TEMP1 1048576',
    'Inc TEMP0',
    'Equal TEMP5 OBJECTVALUE7',
    'Sub TEMP5 OBJECTVALUE6',
    'Sub TEMP5 OBJECTVALUE2',
    'Equal TEMP1 OBJECTVALUE7',
    'Sub TEMP1 524288',
    'Equal TEMP4 524288',
    'Equal OBJECTVALUE22 OBJECTPROPERTYVALUE',
    'Dec OBJECTVALUE22',
    'WLower 2 TEMP0 OBJECTPROPERTYVALUE',
    'Equal TEMP3 TEMP4',
    'ShL TEMP3 7',
    'Div TEMP3 TEMP5',
    'Sin TEMP2 TEMP3',
    'Mul TEMP2 OBJECTVALUE4',
    'ShR TEMP2 9',
    'Add TEMP2 OBJECTYPOS',
    'CallFunction 114',
    'Sub TEMP1 1048576',
    'Add TEMP4 1048576',
    'Inc TEMP0',
    'Dec OBJECTVALUE22',
    'loop',
    'End',
]


# Zone08 function 114 (a bridge log: its frame, then DrawSpriteXY), inlined by PS1HPZBridgeDraw.
HPZBRIDGE_LOG_FN_SIG = [
    'Equal OBJECTFRAME 0',
    'IfEqual 0 OBJECTVALUE22 OBJECTVALUE9',
    'Equal OBJECTFRAME OBJECTVALUE19',
    'endif',
    'IfEqual 2 OBJECTVALUE22 OBJECTVALUE10',
    'Equal OBJECTFRAME OBJECTVALUE19',
    'endif',
    'IfEqual 4 OBJECTVALUE22 OBJECTVALUE12',
    'Equal OBJECTFRAME OBJECTVALUE21',
    'endif',
    'IfEqual 6 OBJECTVALUE22 OBJECTVALUE13',
    'Equal OBJECTFRAME OBJECTVALUE21',
    'endif',
    'IfEqual 8 OBJECTVALUE22 OBJECTVALUE8',
    'Equal OBJECTFRAME OBJECTVALUE18',
    'endif',
    'IfEqual 10 OBJECTVALUE22 OBJECTVALUE11',
    'Equal OBJECTFRAME OBJECTVALUE20',
    'endif',
    'DrawSpriteXY OBJECTFRAME TEMP1 TEMP2',
    'return',
]


# Stage Setup update sub (GlobalCode, every zone; its jump-table entries included), transcribed by PS1StageSetupUpdate.
STAGESETUP_SIG = [
    'switch 0 STAGESTATE',
    'break',
    'Inc GLOBAL[1,0,18]',
    'IfEqual 6 GLOBAL[1,0,18] 4',
    'Equal GLOBAL[1,0,18] 0',
    'Inc GLOBAL[1,0,19]',
    'And GLOBAL[1,0,19] 7',
    'endif',
    'Inc LOCAL[1,0,24725]',
    'IfGreater 8 LOCAL[1,0,24725] 17',
    'Equal LOCAL[1,0,24725] 0',
    'endif',
    'IfNotEqual 10 GLOBAL[1,0,0] 2',
    'IfGreaterOrEqual 12 GLOBAL[1,0,22] GLOBAL[1,0,24]',
    'Inc GLOBAL[1,0,25]',
    'Add GLOBAL[1,0,24] 50000',
    'PlaySfx 24 0',
    'PauseMusic',
    'ResetObjectEntity 25 34 2 0 0',
    'Equal OBJECTPRIORITY[1,0,25] 1',
    'endif',
    'endif',
    'Inc GLOBAL[1,0,17]',
    'And GLOBAL[1,0,17] 511',
    'CallFunction 71',
    'IfEqual 14 STAGETIMEENABLED 1',
    'IfEqual 16 STAGEMINUTES 10',
    'CheckEqual STAGEDEBUGMODE 1',
    'Equal TEMP0 CHECKRESULT',
    'CheckEqual GLOBAL[1,0,0] 2',
    'Or TEMP0 CHECKRESULT',
    'IfEqual 18 TEMP0 0',
    'Equal GLOBAL[1,0,52] 1',
    'Equal ARRAYPOS6 0',
    'Equal OBJECTTYPE[1,0,0] 1',
    'CallFunction 50',
    'endif',
    'Equal STAGEMINUTES 9',
    'Equal STAGESECONDS 59',
    'Equal STAGEMILLISECONDS 99',
    'Equal STAGETIMEENABLED 0',
    'endif',
    'endif',
    'ForEachActive 20 256 ARRAYPOS6',
    'Equal TEMP0 OBJECTCOLLISIONLEFT[1,1,6]',
    'ShL TEMP0 16',
    'Add TEMP0 OBJECTXPOS[1,1,6]',
    'Equal TEMP1 STAGECURXBOUNDARY1',
    'ShL TEMP1 16',
    'IfLower 22 TEMP0 TEMP1',
    'IfEqual 24 OBJECTRIGHT[1,1,6] 1',
    'Equal OBJECTXVEL[1,1,6] 65536',
    'Equal OBJECTSPEED[1,1,6] 65536',
    'else',
    'Equal OBJECTXVEL[1,1,6] 0',
    'Equal OBJECTSPEED[1,1,6] 0',
    'endif',
    'Equal OBJECTXPOS[1,1,6] TEMP1',
    'Equal TEMP0 OBJECTCOLLISIONLEFT[1,1,6]',
    'ShL TEMP0 16',
    'Sub OBJECTXPOS[1,1,6] TEMP0',
    'endif',
    'Equal TEMP1 STAGECURYBOUNDARY2',
    'ShL TEMP1 16',
    'IfLower 26 TEMP1 GLOBAL[1,0,49]',
    'IfGreater 28 OBJECTYPOS[1,1,6] GLOBAL[1,0,49]',
    'CallFunction 50',
    'endif',
    'else',
    'IfGreater 30 OBJECTYPOS[1,1,6] TEMP1',
    'CallFunction 50',
    'endif',
    'endif',
    'next',
    'endswitch',
    'IfEqual 32 GLOBAL[1,0,5] 0',
    'IfGreater 34 OBJECTCONTROLMODE[1,0,0] -1',
    'Equal GLOBAL[1,0,7] 1',
    'else',
    'Equal GLOBAL[1,0,7] 0',
    'endif',
    'else',
    'Equal GLOBAL[1,0,7] 0',
    'endif',
    'Equal ARRAYPOS6 ARRAYPOS7',
    'Dec ARRAYPOS6',
    'WGreater 36 ARRAYPOS6 -1',
    'IfEqual 38 OBJECTVISIBLE[1,1,6] 1',
    'Add ARRAYPOS6 ARRAYPOS7',
    'IfEqual 40 OBJECTVALUE18[1,1,6] 0',
    'Sub ARRAYPOS6 ARRAYPOS7',
    'Equal ARRAYPOS0 OBJECTVALUE18[1,1,6]',
    'AddDrawListEntityRef ARRAYPOS0 ARRAYPOS6',
    'Add ARRAYPOS6 ARRAYPOS7',
    'AddDrawListEntityRef ARRAYPOS0 ARRAYPOS6',
    'Sub ARRAYPOS6 ARRAYPOS7',
    'else',
    'Sub ARRAYPOS6 ARRAYPOS7',
    'Equal ARRAYPOS0 OBJECTVALUE18[1,1,6]',
    'Add ARRAYPOS6 ARRAYPOS7',
    'AddDrawListEntityRef ARRAYPOS0 ARRAYPOS6',
    'Sub ARRAYPOS6 ARRAYPOS7',
    'Equal ARRAYPOS0 OBJECTVALUE18[1,1,6]',
    'AddDrawListEntityRef ARRAYPOS0 ARRAYPOS6',
    'endif',
    'endif',
    'Dec ARRAYPOS6',
    'loop',
    'End',
    'JT 0 switch 2 3 @2 @75 @1 @1',
    'JT 3 IfEqual @7 @8',
    'JT 9 IfGreater @11 @12',
    'JT 12 IfNotEqual @21 @22',
    'JT 13 IfGreaterOrEqual @20 @21',
    'JT 25 IfEqual @42 @43',
    'JT 26 IfEqual @41 @42',
    'JT 31 IfEqual @36 @37',
    'JT 43 ForEachActive @43 @74',
    'JT 49 IfLower @61 @62',
    'JT 50 IfEqual @54 @57',
    'JT 64 IfLower @69 @73',
    'JT 65 IfGreater @67 @68',
    'JT 69 IfGreater @71 @72',
    'JT 75 IfEqual @82 @84',
    'JT 76 IfGreater @79 @81',
    'JT 86 WGreater @86 @108',
    'JT 87 IfEqual @105 @106',
    'JT 89 IfEqual @97 @105',
]


# GlobalCode function 72 (the HUD: score, time, rings, lives), transcribed by PS1HUDDraw.
HUD_FN_SIG = [
    'DrawSpriteScreenXY 10 17 13',
    'IfEqual 0 STAGEMINUTES 9',
    'IfGreater 2 OBJECTVALUE1 7',
    'DrawSpriteScreenXY 11 17 29',
    'endif',
    'endif',
    'IfEqual 4 OBJECTVALUE0[1,0,0] 0',
    'IfGreater 6 OBJECTVALUE1 7',
    'DrawSpriteScreenXY 12 17 45',
    'endif',
    'endif',
    'DrawNumbers 0 104 13 GLOBAL[1,0,22] 6 8 0',
    'IfLower 8 GLOBAL[1,0,0] 2',
    'DrawSpriteScreenXY 14 67 29',
    'else',
    'DrawSpriteScreenXY 13 67 29',
    'DrawNumbers 0 104 29 STAGEMILLISECONDS 2 8 1',
    'endif',
    'DrawNumbers 0 80 29 STAGESECONDS 2 8 1',
    'DrawNumbers 0 56 29 STAGEMINUTES 1 8 1',
    'DrawNumbers 0 80 45 OBJECTVALUE0[1,0,0] 3 8 0',
    'IfEqual 10 STAGEDEBUGMODE 1',
    'Equal TEMP0 OBJECTXPOS[1,0,0]',
    'ShR TEMP0 16',
    'Abs TEMP0',
    'Equal TEMP1 SCREENXSIZE',
    'Sub TEMP1 24',
    'DrawNumbers 0 TEMP1 13 TEMP0 5 8 1',
    'Equal TEMP0 OBJECTYPOS[1,0,0]',
    'ShR TEMP0 16',
    'Abs TEMP0',
    'DrawNumbers 0 TEMP1 29 TEMP0 5 8 1',
    'Sub TEMP1 42',
    'IfGreaterOrEqual 12 OBJECTXPOS[1,0,0] 0',
    'DrawSpriteScreenXY 18 TEMP1 15',
    'else',
    'DrawSpriteScreenXY 36 TEMP1 15',
    'endif',
    'IfGreaterOrEqual 14 OBJECTYPOS[1,0,0] 0',
    'DrawSpriteScreenXY 19 TEMP1 31',
    'else',
    'DrawSpriteScreenXY 37 TEMP1 31',
    'endif',
    'endif',
    'Equal TEMP0 STAGEPLAYERLISTPOS',
    'Add TEMP0 15',
    'DrawSpriteScreenXY TEMP0 16 212',
    'Add TEMP0 6',
    'DrawSpriteScreenXY TEMP0 33 213',
    'DrawSpriteScreenXY 20 38 222',
    'DrawNumbers 24 56 220 GLOBAL[1,0,25] 2 8 0',
    'return',
]


# Wing Fortress Turret Platform update sub (with its jump-table entries), transcribed by PS1TurretPlatform.
TURRET_SIG = [
    'switch 0 OBJECTSTATE',
    'Equal TEMP0 GLOBAL[1,0,17]',
    'And TEMP0 240',
    'IfEqual 8 TEMP0 OBJECTPROPERTYVALUE',
    'Equal OBJECTPRIORITY 1',
    'Equal OBJECTVALUE0 160',
    'Equal OBJECTANIMATIONTIMER 6',
    'Equal OBJECTSTATE 1',
    'endif',
    'break',
    'Inc OBJECTANIMATIONTIMER',
    'Equal OBJECTFRAME OBJECTANIMATIONTIMER',
    'Div OBJECTFRAME 6',
    'IfEqual 10 OBJECTFRAME 4',
    'Equal OBJECTSTATE 2',
    'endif',
    'break',
    'Dec OBJECTVALUE0',
    'IfEqual 12 OBJECTVALUE0 0',
    'Equal OBJECTSTATE 3',
    'endif',
    'break',
    'Dec OBJECTANIMATIONTIMER',
    'Equal OBJECTFRAME OBJECTANIMATIONTIMER',
    'Div OBJECTFRAME 6',
    'IfEqual 14 OBJECTANIMATIONTIMER 0',
    'Equal OBJECTSTATE 0',
    'endif',
    'break',
    'endswitch',
    'ForEachActive 16 256 ARRAYPOS6',
    'IfGreaterOrEqual 18 OBJECTFRAME 3',
    'BoxCollisionTest 3 OBJECTENTITYPOS -32 -24 32 0 ARRAYPOS6 65536 65536 65536 65536',
    'endif',
    'next',
    'IfEqual 20 OBJECTOUTOFBOUNDS 1',
    'Equal ARRAYPOS0 OBJECTENTITYPOS',
    'CallFunction 121',
    'endif',
    'End',
    'JT 0 switch 0 3 @29 @30 @1 @10 @17 @22',
    'JT 3 IfEqual @8 @9',
    'JT 13 IfEqual @15 @16',
    'JT 18 IfEqual @20 @21',
    'JT 25 IfEqual @27 @28',
    'JT 30 ForEachActive @30 @35',
    'JT 31 IfGreaterOrEqual @33 @34',
    'JT 35 IfEqual @38 @39',
]


# Zone11 function 121 (the turret platform reset), inlined by PS1TurretPlatform.
TURRET_FN_SIG = [
    'Equal OBJECTVALUE0[1,1,0] 0',
    'Equal OBJECTANIMATIONTIMER[1,1,0] 0',
    'Equal OBJECTFRAME[1,1,0] 0',
    'Equal OBJECTSTATE[1,1,0] 0',
    'Equal OBJECTPRIORITY[1,1,0] 1',
    'return',
]


# Wing Fortress Belt Platform update sub (with its jump-table entries), transcribed by PS1BeltPlatform.
BELTPLAT_SIG = [
    'switch 0 OBJECTSTATE',
    'break',
    'Inc OBJECTANIMATIONTIMER',
    'IfEqual 8 OBJECTANIMATIONTIMER 4',
    'Equal OBJECTANIMATIONTIMER 0',
    'Dec OBJECTFRAME',
    'IfEqual 10 OBJECTFRAME 0',
    'Equal OBJECTSTATE 2',
    'endif',
    'endif',
    'break',
    'Add OBJECTYPOS OBJECTYVEL',
    'Dec OBJECTVALUE0',
    'IfEqual 12 OBJECTVALUE0 0',
    'Equal OBJECTSTATE 3',
    'endif',
    'Equal TEMP2 0',
    'ForEachActive 14 256 ARRAYPOS6',
    'GetBit TEMP0 OBJECTVALUE1 TEMP2',
    'IfEqual 16 TEMP0 1',
    'Add OBJECTYPOS[1,1,6] OBJECTYVEL',
    'endif',
    'SetBit OBJECTVALUE1 TEMP2 0',
    'BoxCollisionTest 3 OBJECTENTITYPOS -24 -4 24 4 ARRAYPOS6 65536 65536 65536 65536',
    'IfEqual 18 CHECKRESULT 1',
    'SetBit OBJECTVALUE1 TEMP2 1',
    'endif',
    'Inc TEMP2',
    'next',
    'break',
    'Inc OBJECTANIMATIONTIMER',
    'IfEqual 20 OBJECTANIMATIONTIMER 4',
    'Equal OBJECTANIMATIONTIMER 0',
    'Inc OBJECTFRAME',
    'IfEqual 22 OBJECTFRAME 3',
    'Equal OBJECTSTATE 0',
    'endif',
    'endif',
    'break',
    'endswitch',
    'Equal TEMP0 OBJECTXPOS',
    'ShR TEMP0 16',
    'Sub TEMP0 SCREENCAMERAX',
    'Abs TEMP0',
    'Sub TEMP0 128',
    'IfGreater 24 TEMP0 SCREENXCENTER',
    'Equal OBJECTSTATE 0',
    'Equal OBJECTPRIORITY 0',
    'endif',
    'End',
    'JT 0 switch 0 3 @39 @40 @1 @2 @11 @30',
    'JT 3 IfEqual @9 @10',
    'JT 6 IfEqual @8 @9',
    'JT 13 IfEqual @15 @16',
    'JT 17 ForEachActive @17 @29',
    'JT 19 IfEqual @21 @22',
    'JT 24 IfEqual @26 @27',
    'JT 31 IfEqual @37 @38',
    'JT 34 IfEqual @36 @37',
    'JT 45 IfGreater @48 @49',
]


# Casino Night H Flipper update sub (with its jump-table entries), transcribed by PS1HFlipper.
HFLIPPER_SIG = [
    'switch 0 OBJECTSTATE',
    'Equal OBJECTFRAME OBJECTVALUE0',
    'ShR OBJECTFRAME 2',
    'Inc OBJECTFRAME',
    'IfLower 5 OBJECTVALUE0 12',
    'Inc OBJECTVALUE0',
    'else',
    'Equal OBJECTVALUE0 0',
    'Equal OBJECTFRAME 0',
    'Equal OBJECTSTATE 0',
    'endif',
    'break',
    'endswitch',
    'ForEachActive 7 256 ARRAYPOS6',
    'CheckEqual OBJECTSTATE[1,1,6] 28',
    'Equal TEMP0 CHECKRESULT',
    'CheckEqual OBJECTSTATE[1,1,6] 26',
    'Or TEMP0 CHECKRESULT',
    'IfEqual 9 TEMP0 0',
    'Equal OBJECTVALUE4 OBJECTXPOS[1,1,6]',
    'Sub OBJECTVALUE4 OBJECTXPOS',
    'ShR OBJECTVALUE4 16',
    'Equal OBJECTVALUE3 OBJECTVALUE4',
    'Add OBJECTVALUE3 OBJECTCOLLISIONLEFT[1,1,6]',
    'Equal OBJECTVALUE5 OBJECTVALUE4',
    'Add OBJECTVALUE5 OBJECTCOLLISIONRIGHT[1,1,6]',
    'Add OBJECTVALUE3 24',
    'IfLower 11 OBJECTVALUE3 0',
    'Equal OBJECTVALUE3 0',
    'endif',
    'IfGreater 13 OBJECTVALUE3 47',
    'Equal OBJECTVALUE3 47',
    'endif',
    'Add OBJECTVALUE4 24',
    'IfLower 15 OBJECTVALUE4 0',
    'Equal OBJECTVALUE4 0',
    'endif',
    'IfGreater 17 OBJECTVALUE4 47',
    'Equal OBJECTVALUE4 47',
    'endif',
    'Add OBJECTVALUE5 24',
    'IfLower 19 OBJECTVALUE5 0',
    'Equal OBJECTVALUE5 0',
    'endif',
    'IfGreater 21 OBJECTVALUE5 47',
    'Equal OBJECTVALUE5 47',
    'endif',
    'GetTableValue OBJECTVALUE6 OBJECTVALUE3 OBJECTVALUE1',
    'GetTableValue OBJECTVALUE7 OBJECTVALUE4 OBJECTVALUE1',
    'GetTableValue OBJECTVALUE8 OBJECTVALUE5 OBJECTVALUE1',
    'Equal OBJECTVALUE9 OBJECTVALUE6',
    'IfLower 23 OBJECTVALUE7 OBJECTVALUE9',
    'Equal OBJECTVALUE9 OBJECTVALUE7',
    'endif',
    'IfLower 25 OBJECTVALUE8 OBJECTVALUE9',
    'Equal OBJECTVALUE9 OBJECTVALUE8',
    'endif',
    'GetTableValue OBJECTVALUE6 OBJECTVALUE3 OBJECTVALUE2',
    'GetTableValue OBJECTVALUE7 OBJECTVALUE4 OBJECTVALUE2',
    'GetTableValue OBJECTVALUE8 OBJECTVALUE5 OBJECTVALUE2',
    'Equal OBJECTVALUE10 OBJECTVALUE6',
    'IfGreater 27 OBJECTVALUE7 OBJECTVALUE10',
    'Equal OBJECTVALUE10 OBJECTVALUE7',
    'endif',
    'IfGreater 29 OBJECTVALUE8 OBJECTVALUE10',
    'Equal OBJECTVALUE10 OBJECTVALUE8',
    'endif',
    'IfNotEqual 31 OBJECTSTATE[1,1,6] 1',
    'BoxCollisionTest 1 OBJECTENTITYPOS -24 OBJECTVALUE9 24 OBJECTVALUE10 ARRAYPOS6 65536 65536 65536 65536',
    'IfEqual 33 CHECKRESULT 4',
    'CheckEqual OBJECTSTATE[1,1,6] 24',
    'Equal TEMP0 CHECKRESULT',
    'CheckEqual OBJECTSTATE[1,1,6] 20',
    'Or TEMP0 CHECKRESULT',
    'CheckEqual OBJECTSTATE[1,1,6] 21',
    'Or TEMP0 CHECKRESULT',
    'CheckEqual OBJECTSTATE[1,1,6] 19',
    'Or TEMP0 CHECKRESULT',
    'CheckEqual OBJECTSTATE[1,1,6] 31',
    'Or TEMP0 CHECKRESULT',
    'CheckEqual OBJECTSTATE[1,1,6] 13',
    'Or TEMP0 CHECKRESULT',
    'IfEqual 35 TEMP0 0',
    'Equal OBJECTSTATE[1,1,6] 12',
    'Equal OBJECTGRAVITY[1,1,6] 1',
    'endif',
    'endif',
    'Equal TEMP0 4',
    'else',
    'Equal TEMP0 12',
    'endif',
    'Sub OBJECTVALUE9 TEMP0',
    'Equal OBJECTVALUE10 OBJECTVALUE9',
    'Add OBJECTVALUE10 8',
    'BoxCollisionTest 0 OBJECTENTITYPOS -24 OBJECTVALUE9 24 OBJECTVALUE10 ARRAYPOS6 65536 65536 65536 65536',
    'IfEqual 37 CHECKRESULT 1',
    'Equal OBJECTYPOS[1,1,6] OBJECTVALUE9',
    'Add OBJECTYPOS[1,1,6] TEMP0',
    'Sub OBJECTYPOS[1,1,6] OBJECTCOLLISIONBOTTOM[1,1,6]',
    'ShL OBJECTYPOS[1,1,6] 16',
    'Add OBJECTYPOS[1,1,6] OBJECTYPOS',
    'IfEqual 39 OBJECTSTATE 0',
    'IfEqual 41 OBJECTDIRECTION 0',
    'BoxCollisionTest 0 OBJECTENTITYPOS -26 -12 20 8 ARRAYPOS6 65536 65536 65536 65536',
    'else',
    'BoxCollisionTest 0 OBJECTENTITYPOS -20 -12 26 8 ARRAYPOS6 65536 65536 65536 65536',
    'endif',
    'IfEqual 43 CHECKRESULT 1',
    'CheckEqual OBJECTSTATE[1,1,6] 31',
    'Equal TEMP0 CHECKRESULT',
    'CheckEqual OBJECTSTATE[1,1,6] 13',
    'Or TEMP0 CHECKRESULT',
    'IfEqual 45 TEMP0 0',
    'Equal OBJECTSTATE[1,1,6] 14',
    'Equal OBJECTANIMATION[1,1,6] GLOBAL[1,0,67]',
    'endif',
    'Equal OBJECTSCROLLTRACKING[1,1,6] 0',
    'Equal OBJECTGRAVITY[1,1,6] 0',
    'Equal OBJECTCONTROLLOCK[1,1,6] 8',
    'IfEqual 47 OBJECTJUMPPRESS[1,1,6] 1',
    'CheckEqual OBJECTSTATE[1,1,6] 31',
    'Equal TEMP0 CHECKRESULT',
    'CheckEqual OBJECTSTATE[1,1,6] 13',
    'Or TEMP0 CHECKRESULT',
    'IfEqual 49 TEMP0 0',
    'Equal OBJECTSTATE[1,1,6] 12',
    'endif',
    'Equal OBJECTGRAVITY[1,1,6] 1',
    'Equal OBJECTVALUE1[1,1,6] 0',
    'Equal OBJECTCONTROLLOCK[1,1,6] 0',
    'CallFunction 122',
    'Equal OBJECTSTATE 1',
    'StopSfx 0',
    'PlaySfx 48 0',
    'else',
    'Equal OBJECTSPEED[1,1,6] OBJECTXVEL',
    'Equal OBJECTXVEL[1,1,6] OBJECTXVEL',
    'Equal OBJECTYVEL[1,1,6] 0',
    'endif',
    'else',
    'CheckEqual OBJECTSTATE[1,1,6] 31',
    'Equal TEMP0 CHECKRESULT',
    'CheckEqual OBJECTSTATE[1,1,6] 13',
    'Or TEMP0 CHECKRESULT',
    'IfEqual 51 TEMP0 0',
    'Equal OBJECTSTATE[1,1,6] 12',
    'endif',
    'Equal OBJECTGRAVITY[1,1,6] 1',
    'Equal OBJECTVALUE1[1,1,6] 0',
    'endif',
    'Equal OBJECTANIMATION[1,1,6] GLOBAL[1,0,67]',
    'IfNotEqual 53 OBJECTPREVANIMATION[1,1,6] GLOBAL[1,0,67]',
    'Sub OBJECTIYPOS[1,1,6] OBJECTVALUE30[1,1,6]',
    'endif',
    'else',
    'CheckEqual OBJECTSTATE[1,1,6] 31',
    'Equal TEMP0 CHECKRESULT',
    'CheckEqual OBJECTSTATE[1,1,6] 13',
    'Or TEMP0 CHECKRESULT',
    'IfEqual 55 TEMP0 0',
    'Equal OBJECTSTATE[1,1,6] 12',
    'endif',
    'CallFunction 122',
    'Equal OBJECTGRAVITY[1,1,6] 1',
    'Equal OBJECTVALUE1[1,1,6] 0',
    'endif',
    'endif',
    'endif',
    'next',
    'End',
    'JT 0 switch 1 1 @12 @13 @1',
    'JT 4 IfLower @7 @11',
    'JT 13 ForEachActive @13 @169',
    'JT 18 IfEqual @167 @168',
    'JT 27 IfLower @29 @30',
    'JT 30 IfGreater @32 @33',
    'JT 34 IfLower @36 @37',
    'JT 37 IfGreater @39 @40',
    'JT 41 IfLower @43 @44',
    'JT 44 IfGreater @46 @47',
    'JT 51 IfLower @53 @54',
    'JT 54 IfLower @56 @57',
    'JT 61 IfGreater @63 @64',
    'JT 64 IfGreater @66 @67',
    'JT 67 IfNotEqual @89 @91',
    'JT 69 IfEqual @86 @87',
    'JT 82 IfEqual @85 @86',
    'JT 95 IfEqual @166 @167',
    'JT 101 IfEqual @155 @166',
    'JT 102 IfEqual @105 @107',
    'JT 107 IfEqual @140 @150',
    'JT 112 IfEqual @115 @116',
    'JT 119 IfEqual @135 @139',
    'JT 124 IfEqual @126 @127',
    'JT 144 IfEqual @146 @147',
    'JT 151 IfNotEqual @153 @154',
    'JT 159 IfEqual @161 @162',
]


# Zone04 function 122 (the flipper launch), inlined by PS1HFlipper.
HFLIPPER_FN_SIG = [
    'Equal TEMP0 OBJECTIXPOS[1,1,6]',
    'Sub TEMP0 OBJECTIXPOS',
    'IfEqual 0 OBJECTDIRECTION 1',
    'FlipSign TEMP0',
    'endif',
    'Add TEMP0 35',
    'Equal TEMP2 TEMP0',
    'IfGreater 2 TEMP2 64',
    'Equal TEMP2 64',
    'endif',
    'ShL TEMP2 5',
    'Add TEMP2 2048',
    'FlipSign TEMP2',
    'Equal TEMP3 TEMP0',
    'ShR TEMP3 2',
    'Add TEMP3 64',
    'Sin256 TEMP0 TEMP3',
    'Cos256 TEMP1 TEMP3',
    'Mul TEMP0 TEMP2',
    'Mul TEMP1 TEMP2',
    'Equal OBJECTYVEL[1,1,6] TEMP0',
    'IfEqual 4 OBJECTDIRECTION 1',
    'FlipSign TEMP1',
    'endif',
    'Equal OBJECTSPEED[1,1,6] TEMP1',
    'Equal OBJECTXVEL[1,1,6] TEMP1',
    'return',
]


# Hill Top Earthquake update sub (with its jump-table entries), transcribed by PS1Earthquake (calls functions 46 / 50).
EARTHQUAKE_SIG = [
    'BoxCollisionTest 0 OBJECTENTITYPOS OBJECTVALUE7 OBJECTVALUE8 OBJECTVALUE9 OBJECTVALUE10 0 0 0 0 0',
    'IfEqual 0 CHECKRESULT 1',
    'IfEqual 2 LOCAL[1,0,63679] 0',
    'Equal LOCAL[1,0,63679] 2',
    'Equal LOCAL[1,0,63680] OBJECTVALUE0',
    'Equal LOCAL[1,0,63681] OBJECTVALUE1',
    'Equal LOCAL[1,0,63682] OBJECTVALUE2',
    'Equal HPARALLAXSCROLLPOS[1,0,20] OBJECTVALUE16',
    'endif',
    'endif',
    'BoxCollisionTest 0 OBJECTENTITYPOS OBJECTVALUE3 OBJECTVALUE4 OBJECTVALUE5 OBJECTVALUE6 0 0 0 0 0',
    'IfEqual 4 CHECKRESULT 1',
    'IfEqual 6 LOCAL[1,0,63679] 0',
    'Equal LOCAL[1,0,63679] 1',
    'Equal LOCAL[1,0,63680] OBJECTVALUE0',
    'Equal LOCAL[1,0,63681] OBJECTVALUE1',
    'Equal LOCAL[1,0,63682] OBJECTVALUE2',
    'Equal HPARALLAXSCROLLPOS[1,0,20] OBJECTVALUE16',
    'endif',
    'endif',
    'ForEachActive 8 256 ARRAYPOS6',
    'Equal TEMP0 OBJECTVALUE12',
    'Sub TEMP0 OBJECTVALUE15',
    'Equal TEMP1 OBJECTVALUE14',
    'Sub TEMP1 OBJECTVALUE15',
    'IfEqual 10 OBJECTPROPERTYVALUE 3',
    'Equal TEMP2 OBJECTXPOS[1,1,6]',
    'Sub TEMP2 OBJECTXPOS',
    'Add TEMP2 25165824',
    'IfGreater 12 TEMP2 0',
    'ShR TEMP2 18',
    'Add TEMP0 TEMP2',
    'endif',
    'Inc TEMP0',
    'IfEqual 14 OBJECTVALUE42[1,1,6] 0',
    'IfEqual 16 OBJECTGRAVITY[1,1,6] 1',
    'IfGreater 18 OBJECTXVEL[1,1,6] 0',
    'Equal TEMP2 OBJECTXVEL[1,1,6]',
    'ShR TEMP2 2',
    'Add OBJECTYPOS[1,1,6] TEMP2',
    'Add OBJECTYPOS[1,1,6] 131072',
    'endif',
    'endif',
    'endif',
    'endif',
    'BoxCollisionTest 1 OBJECTENTITYPOS OBJECTVALUE11 TEMP0 OBJECTVALUE13 TEMP1 ARRAYPOS6 65536 65536 65536 65536',
    'switch 20 CHECKRESULT',
    'Equal TEMP0 LOCAL[1,0,63683]',
    'Sub TEMP0 OBJECTVALUE15',
    'ShL TEMP0 16',
    'Sub OBJECTYPOS[1,1,6] TEMP0',
    'IfEqual 28 OBJECTPROPERTYVALUE 2',
    'CallFunction 46',
    'endif',
    'IfEqual 30 OBJECTPROPERTYVALUE 4',
    'CallFunction 46',
    'endif',
    'IfEqual 32 OBJECTPROPERTYVALUE 5',
    'CallFunction 46',
    'endif',
    'break',
    'IfEqual 34 OBJECTGRAVITY[1,1,6] 0',
    'CallFunction 50',
    'endif',
    'break',
    'endswitch',
    'next',
    'ForEachActive 36 11 ARRAYPOS0',
    'IfGreaterOrEqual 38 OBJECTYVEL[1,1,0] 0',
    'Equal TEMP0 OBJECTVALUE12',
    'Sub TEMP0 OBJECTVALUE15',
    'Equal TEMP1 OBJECTVALUE14',
    'Sub TEMP1 OBJECTVALUE15',
    'IfEqual 40 OBJECTPROPERTYVALUE 3',
    'Equal TEMP2 OBJECTXPOS[1,1,0]',
    'Sub TEMP2 OBJECTXPOS',
    'Add TEMP2 25165824',
    'IfGreater 42 TEMP2 0',
    'ShR TEMP2 18',
    'Add TEMP0 TEMP2',
    'endif',
    'Inc TEMP0',
    'endif',
    'Equal TEMP2 OBJECTYVEL[1,1,0]',
    'BoxCollisionTest 3 OBJECTENTITYPOS OBJECTVALUE11 TEMP0 OBJECTVALUE13 TEMP1 ARRAYPOS0 -8 -8 8 8',
    'IfEqual 44 CHECKRESULT 1',
    'Equal OBJECTYVEL[1,1,0] TEMP2',
    'ShR TEMP2 2',
    'Sub OBJECTYVEL[1,1,0] TEMP2',
    'FlipSign OBJECTYVEL[1,1,0]',
    'IfGreater 46 OBJECTYVEL[1,1,0] -65536',
    'Equal OBJECTYVEL[1,1,0] -65536',
    'endif',
    'Equal OBJECTGRAVITY[1,1,0] 1',
    'endif',
    'endif',
    'next',
    'Equal OBJECTVALUE15 LOCAL[1,0,63683]',
    'End',
    'JT 1 IfEqual @9 @10',
    'JT 2 IfEqual @8 @9',
    'JT 11 IfEqual @19 @20',
    'JT 12 IfEqual @18 @19',
    'JT 20 ForEachActive @20 @67',
    'JT 25 IfEqual @44 @45',
    'JT 29 IfGreater @32 @33',
    'JT 34 IfEqual @43 @44',
    'JT 35 IfEqual @42 @43',
    'JT 36 IfGreater @41 @42',
    'JT 46 switch 1 4 @65 @66 @47 @51 @51 @61',
    'JT 51 IfEqual @53 @54',
    'JT 54 IfEqual @56 @57',
    'JT 57 IfEqual @59 @60',
    'JT 61 IfEqual @63 @64',
    'JT 67 ForEachActive @67 @97',
    'JT 68 IfGreaterOrEqual @95 @96',
    'JT 73 IfEqual @82 @83',
    'JT 77 IfGreater @80 @81',
    'JT 85 IfEqual @94 @95',
    'JT 90 IfGreater @92 @93',
]


# Spikes update sub (GlobalCode, with its jump-table entries), transcribed by PS1Spikes (calls functions 49 / 50).
SPIKES_SIG = [
    'switch 0 OBJECTSTATE',
    'IfLower 9 OBJECTVALUE0 60',
    'Inc OBJECTVALUE0',
    'else',
    'Inc OBJECTSTATE',
    'Equal OBJECTVALUE0 0',
    'Equal TEMP0 SCREENXCENTER',
    'Add TEMP0 64',
    'Equal TEMP1 TEMP0',
    'FlipSign TEMP0',
    'BoxCollisionTest 0 OBJECTENTITYPOS TEMP0 -128 TEMP1 128 0 0 0 0 0',
    'IfEqual 11 CHECKRESULT 1',
    'PlaySfx 29 0',
    'endif',
    'endif',
    'break',
    'IfLower 13 OBJECTVALUE1 2097152',
    'Add OBJECTVALUE1 524288',
    'Add OBJECTXPOS OBJECTXVEL',
    'Add OBJECTYPOS OBJECTYVEL',
    'else',
    'Inc OBJECTSTATE',
    'endif',
    'break',
    'IfLower 15 OBJECTVALUE0 60',
    'Inc OBJECTVALUE0',
    'else',
    'Inc OBJECTSTATE',
    'Equal OBJECTVALUE0 0',
    'Equal TEMP0 SCREENXCENTER',
    'Add TEMP0 64',
    'Equal TEMP1 TEMP0',
    'FlipSign TEMP0',
    'BoxCollisionTest 0 OBJECTENTITYPOS TEMP0 -128 TEMP1 128 0 0 0 0 0',
    'IfEqual 17 CHECKRESULT 1',
    'PlaySfx 29 0',
    'endif',
    'endif',
    'break',
    'IfGreater 19 OBJECTVALUE1 0',
    'Sub OBJECTVALUE1 524288',
    'Sub OBJECTXPOS OBJECTXVEL',
    'Sub OBJECTYPOS OBJECTYVEL',
    'else',
    'Equal OBJECTSTATE 1',
    'endif',
    'break',
    'Equal ARRAYPOS0 OBJECTVALUE2',
    'Equal OBJECTYPOS OBJECTYPOS[3,1,0]',
    'break',
    'endswitch',
    'switch 21 OBJECTPROPERTYVALUE',
    'ForEachActive 29 256 ARRAYPOS6',
    'BoxCollisionTest 1 OBJECTENTITYPOS -16 -16 16 16 ARRAYPOS6 65536 65536 65536 65536',
    'IfEqual 31 CHECKRESULT 4',
    'IfEqual 33 OBJECTGRAVITY[1,1,6] 0',
    'CallFunction 50',
    'endif',
    'endif',
    'IfGreater 35 OBJECTYVEL[1,1,6] -1',
    'BoxCollisionTest 0 OBJECTENTITYPOS -15 -17 15 -12 ARRAYPOS6 65536 65536 65536 65536',
    'IfEqual 37 CHECKRESULT 1',
    'CallFunction 49',
    'endif',
    'endif',
    'next',
    'break',
    'ForEachActive 39 256 ARRAYPOS6',
    'BoxCollisionTest 1 OBJECTENTITYPOS -16 -16 15 16 ARRAYPOS6 65536 65536 65536 65536',
    'IfEqual 41 CHECKRESULT 4',
    'IfEqual 43 OBJECTGRAVITY[1,1,6] 0',
    'CallFunction 50',
    'endif',
    'endif',
    'IfLowerOrEqual 45 OBJECTXVEL[1,1,6] 0',
    'BoxCollisionTest 0 OBJECTENTITYPOS 12 -15 16 15 ARRAYPOS6 65536 65536 65536 65536',
    'IfEqual 47 CHECKRESULT 1',
    'CallFunction 49',
    'endif',
    'endif',
    'next',
    'break',
    'ForEachActive 49 256 ARRAYPOS6',
    'BoxCollisionTest 1 OBJECTENTITYPOS -15 -16 16 16 ARRAYPOS6 65536 65536 65536 65536',
    'IfEqual 51 CHECKRESULT 4',
    'IfEqual 53 OBJECTGRAVITY[1,1,6] 0',
    'CallFunction 50',
    'endif',
    'endif',
    'BoxCollisionTest 0 OBJECTENTITYPOS -16 -15 -12 15 ARRAYPOS6 65536 65536 65536 65536',
    'IfEqual 55 CHECKRESULT 1',
    'CallFunction 49',
    'endif',
    'next',
    'break',
    'ForEachActive 57 256 ARRAYPOS6',
    'Equal TEMP0 OBJECTYVEL[1,1,6]',
    'BoxCollisionTest 1 OBJECTENTITYPOS -16 -16 16 15 ARRAYPOS6 65536 65536 65536 65536',
    'IfEqual 59 CHECKRESULT 4',
    'IfEqual 61 OBJECTGRAVITY[1,1,6] 0',
    'IfEqual 63 OBJECTCOLLISIONMODE[1,1,6] 0',
    'CallFunction 50',
    'Equal OBJECTVALUE7[1,1,6] 0',
    'else',
    'CallFunction 49',
    'endif',
    'else',
    'CallFunction 49',
    'endif',
    'Equal TEMP0 1',
    'endif',
    'IfLower 65 TEMP0 1',
    'BoxCollisionTest 0 OBJECTENTITYPOS -15 12 15 14 ARRAYPOS6 65536 65536 65536 65536',
    'IfEqual 67 CHECKRESULT 1',
    'CallFunction 49',
    'endif',
    'endif',
    'next',
    'break',
    'endswitch',
    'End',
    'JT 0 switch 1 5 @50 @51 @1 @16 @24 @39 @47',
    'JT 1 IfLower @4 @15',
    'JT 11 IfEqual @13 @14',
    'JT 16 IfLower @21 @23',
    'JT 24 IfLower @27 @38',
    'JT 34 IfEqual @36 @37',
    'JT 39 IfGreater @44 @46',
    'JT 51 switch 0 3 @119 @120 @52 @67 @82 @95',
    'JT 52 ForEachActive @52 @66',
    'JT 54 IfEqual @58 @59',
    'JT 55 IfEqual @57 @58',
    'JT 59 IfGreater @64 @65',
    'JT 61 IfEqual @63 @64',
    'JT 67 ForEachActive @67 @81',
    'JT 69 IfEqual @73 @74',
    'JT 70 IfEqual @72 @73',
    'JT 74 IfLowerOrEqual @79 @80',
    'JT 76 IfEqual @78 @79',
    'JT 82 ForEachActive @82 @94',
    'JT 84 IfEqual @88 @89',
    'JT 85 IfEqual @87 @88',
    'JT 90 IfEqual @92 @93',
    'JT 95 ForEachActive @95 @118',
    'JT 98 IfEqual @110 @111',
    'JT 99 IfEqual @107 @109',
    'JT 100 IfEqual @104 @106',
    'JT 111 IfLower @116 @117',
    'JT 113 IfEqual @115 @116',
]


LOOP_OPS = ('WEqual', 'WGreater', 'WGreaterOrEqual', 'WLower', 'WLowerOrEqual', 'WNotEqual', 'switch', 'ForEachActive',
            'ForEachAll')
IF_OPS = ('IfEqual', 'IfGreater', 'IfGreaterOrEqual', 'IfLower', 'IfLowerOrEqual', 'IfNotEqual')
def callable_from_native(path, roots):
    """None when the script functions `roots` (and every function they call) can run from a native opcode in a
    nested ProcessScript (RSDKv4/Script.cpp PS1CallScriptFunction), else the reason. Required: no return from
    inside a while / switch / ForEach (the VM stacks end where the script leaves them), no variable CallFunction
    (every reachable function is checked), no STAGEENTITYPOS (the native keeps its entity)."""
    names, vars_ = bs.tables(3)
    code, _, _, _, fns, _ = bs.full_code(path)
    seen, todo = set(), list(roots)
    while todo:
        f = todo.pop()
        if f in seen:
            continue
        seen.add(f)
        stack = []
        for k, (_, n, ops) in enumerate(instructions(code, fns[f][0], names, vars_)):
            if n in LOOP_OPS or n in IF_OPS:
                stack.append(n)
            elif n in ('loop', 'endswitch', 'next', 'endif') and stack:
                stack.pop()
            elif n == 'return' and any(s in LOOP_OPS for s in stack):
                return 'function %d returns inside a loop / switch' % f
            if n == 'CallFunction':
                if ops[0][0] != 'int':
                    return 'function %d has a variable CallFunction' % f
                todo.append(ops[0][1])
            if any(o[0] == 'var' and o[1][0] == 'STAGEENTITYPOS' for o in ops):
                return 'function %d uses STAGEENTITYPOS' % f
    return None


def patch_global_function(bcdir, sig, op_name):
    """The one GlobalCode function whose listing is exactly `sig` -> op / return."""
    names, vars_ = bs.tables(3)
    fn = [n for n, _ in names]
    op, ret = fn.index(op_name), fn.index('return')
    path = os.path.join(bcdir, 'GlobalCode.bin')
    raw = open(path, 'rb').read()
    code, p = bs.blocks(raw, 0)
    rest = raw[p:]
    found, already = [], []
    for f, (start, _) in enumerate(bs.load(path)[4]):
        if not 0 <= start < len(code):
            continue
        if code[start] == op:
            already.append(f)
            continue
        try:
            if listing(instructions(code, start, names, vars_)) == sig:
                found.append((f, start))
        except (IndexError, KeyError):
            continue
    if already and not found:
        return '%s already native (function %s)' % (op_name, already)
    if len(found) != 1:
        sys.exit('ERROR patch_bytecode: expected one GlobalCode function for %s, found %d' % (op_name, len(found)))
    f, start = found[0]
    code[start:start + 2] = [op, ret]
    open(path, 'wb').write(encode(code) + rest)
    assert bs.blocks(open(path, 'rb').read(), 0)[0] == code
    return '%s native: function %d (%d instructions)' % (op_name, f, len(sig))


def patch_global_sub(bcdir, obj_name, sub, sig, op_name, with_jumps=False, calls=()):
    """GlobalCode object `obj_name`'s sub `sub` (0 update, 1 draw) -> op / End when it is exactly `sig` (with_jumps:
    with its jump-table entries; calls: the script functions the native runs in the VM, callable_from_native)."""
    names, vars_ = bs.tables(3)
    fn = [n for n, _ in names]
    op, end = fn.index(op_name), fn.index('End')
    path = os.path.join(bcdir, 'GlobalCode.bin')
    raw = open(path, 'rb').read()
    code, p = bs.blocks(raw, 0)
    rest = raw[p:]
    start = bs.load(path)[2][global_objects(bcdir).index(obj_name)][sub]
    if code[start] == op:
        return '%s already native' % obj_name
    got = sub_listing(path, global_objects(bcdir).index(obj_name), sub, names, vars_)[3] if with_jumps else \
        listing(instructions(code, start, names, vars_))
    if got != sig:
        sys.exit('ERROR patch_bytecode: %s sub %d is not the one %s transcribes' % (obj_name, sub, op_name))
    why = callable_from_native(path, calls) if calls else None
    if why:
        sys.exit('ERROR patch_bytecode: %s calls script functions it cannot run natively: %s' % (op_name, why))
    code[start:start + 2] = [op, end]
    open(path, 'wb').write(encode(code) + rest)
    assert bs.blocks(open(path, 'rb').read(), 0)[0] == code
    return '%s native (%s sub at %d, %d instructions)' % (obj_name, ('update', 'draw')[sub], start, len(sig))


def patch_ring(bcdir):
    names, vars_ = bs.tables(3)
    fn = [n for n, _ in names]
    op, end = fn.index('PS1Ring'), fn.index('End')
    path = os.path.join(bcdir, 'GlobalCode.bin')
    raw = open(path, 'rb').read()
    code, p = bs.blocks(raw, 0)
    rest = raw[p:]
    obj = global_objects(bcdir).index('Ring')
    start = bs.load(path)[2][obj][0]
    if code[start] == op:
        return 'Ring already native'
    if listing(instructions(code, start, names, vars_)) != RING_SIG:
        sys.exit('ERROR patch_bytecode: the Ring update sub is not the one PS1RingUpdate transcribes')
    code[start:start + 2] = [op, end]
    open(path, 'wb').write(encode(code) + rest)
    assert bs.blocks(open(path, 'rb').read(), 0)[0] == code
    return 'Ring update native (sub at %d, %d instructions)' % (start, len(RING_SIG))


def patch_oscillate(bcdir):
    names, vars_ = bs.tables(3)
    fn = [n for n, _ in names]
    op, ret = fn.index('PS1Oscillate'), fn.index('return')
    path = os.path.join(bcdir, 'GlobalCode.bin')
    raw = open(path, 'rb').read()
    code, p = bs.blocks(raw, 0)
    rest = raw[p:]
    fptrs = [c for c, _ in bs.load(path)[4]]
    found, already = [], []
    for f, start in enumerate(fptrs):
        if not 0 <= start < len(code):
            continue
        if code[start] == op:
            already.append(f)
            continue
        try:
            ins = instructions(code, start, names, vars_)
        except (IndexError, KeyError):
            continue
        if len(ins) != 30 or ins[3][1] != 'GetTableValue':
            continue
        try:
            c = {'n': ins[2][2][2][1], 'params': ins[3][2][2][1], 'state': ins[4][2][2][1], 'bits': ins[9][2][1][1][1][2]}
        except (IndexError, TypeError):
            continue
        got = [(n, [(o[0], o[1]) for o in ops]) for _, n, ops in ins]
        if got == osc_pattern(c):
            found.append((f, start, c))
    if already and not found:
        return 'oscillators already native (function %s)' % already
    if len(found) != 1:
        sys.exit('ERROR patch_bytecode: expected one oscillator function in GlobalCode.bin, found %d' % len(found))
    f, start, c = found[0]
    code[start:start + 10] = [op, 2, c['state'], 2, c['params'], 2, c['bits'], 2, c['n'], ret]
    open(path, 'wb').write(encode(code) + rest)
    assert bs.blocks(open(path, 'rb').read(), 0)[0] == code
    return 'oscillators native: function %d (state %d, params %d, bits local %d, %d oscillators)' % (
        f, c['state'], c['params'], c['bits'], c['n'])


def main():
    bcdir = sys.argv[1]
    only = os.environ.get('PS1_PATCH_ONLY')  # tests: e.g. "oscillate" (a comma list of the patch names below)
    for name, fn in (('oscillate', patch_oscillate), ('ring', patch_ring),
                     ('bridge', lambda d: patch_stage_sub(d, 'Bridge', 1, BRIDGE_SIG, 'PS1BridgeDraw')),
                     ('losering', lambda d: patch_global_sub(d, 'Lose Ring', 0, LOSERING_SIG, 'PS1LoseRing')),
                     ('hdoor', lambda d: patch_stage_sub(d, 'Horizontal Door', 0, HDOOR_SIG, 'PS1HorizontalDoor')),
                     ('buttonbridge', lambda d: patch_stage_sub(d, 'Button Bridge', 0, BUTTONBRIDGE_SIG, 'PS1ButtonBridge',
                                                                with_jumps=True)),
                     ('planesw', lambda d: patch_global_sub(d, 'Plane Sw V', 0, PLANESWV_SIG, 'PS1PlaneSwitchV') + ', ' +
                      patch_global_sub(d, 'Plane Sw H', 0, PLANESWH_SIG, 'PS1PlaneSwitchH')),
                     ('rotplat', lambda d: patch_stage_sub(d, 'Rotate Platform', 0, ROTPLAT_SIG, 'PS1RotatePlatform',
                                                           funcs={45: PUSHPLAYER_FN_SIG}) + ', ' +
                      patch_stage_sub(d, 'Rotate Platform', 1, ROTPLAT_DRAW_SIG, 'PS1RotatePlatformDraw')),
                     ('hpzbridge', lambda d: patch_stage_sub(d, 'Bridge', 0, HPZBRIDGE_SIG, 'PS1HPZBridge') + ', ' +
                      patch_stage_sub(d, 'Bridge', 1, HPZBRIDGE_DRAW_SIG, 'PS1HPZBridgeDraw',
                                      funcs={114: HPZBRIDGE_LOG_FN_SIG})),
                     ('stagesetup', lambda d: patch_global_sub(d, 'Stage Setup', 0, STAGESETUP_SIG, 'PS1StageSetup',
                                                               with_jumps=True, calls=(71, 50))),
                     ('hud', lambda d: patch_global_function(d, HUD_FN_SIG, 'PS1HUDDraw')),
                     ('spikes', lambda d: patch_global_sub(d, 'Spikes', 0, SPIKES_SIG, 'PS1Spikes', with_jumps=True,
                                                           calls=(49, 50))),
                     ('turret', lambda d: patch_stage_sub(d, 'Turret Platform', 0, TURRET_SIG, 'PS1TurretPlatform',
                                                          with_jumps=True, funcs={121: TURRET_FN_SIG})),
                     ('beltplat', lambda d: patch_stage_sub(d, 'Belt Platform', 0, BELTPLAT_SIG, 'PS1BeltPlatform',
                                                            with_jumps=True)),
                     ('hflipper', lambda d: patch_stage_sub(d, 'H Flipper', 0, HFLIPPER_SIG, 'PS1HFlipper', with_jumps=True,
                                                            funcs={122: HFLIPPER_FN_SIG})),
                     ('earthquake', lambda d: patch_stage_sub(d, 'Earthquake', 0, EARTHQUAKE_SIG, 'PS1Earthquake',
                                                              with_jumps=True, calls=(46, 50)))):
        if not only or name in only.split(','):
            print('%s: %s' % (name, fn(bcdir)))


if __name__ == '__main__':
    main()
