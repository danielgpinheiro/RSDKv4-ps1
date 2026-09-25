#!/usr/bin/env python3
"""PS1 edits to Sonic 2's script bytecode, applied to the disc tree at build time (docs/30; the model is the Sonic
CD port's tool). The engine runs the scripts unchanged; only their data changes here: hot script loops become one
PS1 engine opcode (RSDKv4/Script.cpp, `#if RETRO_PLATFORM == RETRO_PS1` FunctionInfo list) that runs the same loop
natively, leaving the same temps and table / local writes behind.

- Oscillators (GlobalCode, the function Stage Setup calls every frame; ~390 VM instructions, ~90 hblanks a frame
  on the emulator): its body -> PS1Oscillate state, params, bits, count / return.
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
    for name, fn in (('oscillate', patch_oscillate), ('ring', patch_ring)):
        if not only or name in only.split(','):
            print('GlobalCode.bin: ' + fn(bcdir))


if __name__ == '__main__':
    main()
