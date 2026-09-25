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


def patch_stage_sub(bcdir, obj_name, sub, sig, op_name, with_jumps=False):
    """Every stage file whose object `obj_name` has sub `sub` (0 update, 1 draw) equal to `sig` -> op / End
    (with_jumps: the signature also holds the sub's jump-table entries, jump_listing)."""
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


def patch_global_sub(bcdir, obj_name, sub, sig, op_name):
    """GlobalCode object `obj_name`'s sub `sub` (0 update, 1 draw) -> op / End when it is exactly `sig`."""
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
    if listing(instructions(code, start, names, vars_)) != sig:
        sys.exit('ERROR patch_bytecode: %s sub %d is not the one %s transcribes' % (obj_name, sub, op_name))
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
                                                                with_jumps=True))):
        if not only or name in only.split(','):
            print('%s: %s' % (name, fn(bcdir)))


if __name__ == '__main__':
    main()
