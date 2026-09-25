#!/usr/bin/env python3
"""Check the PS1 int16 script code (RSDKv4/Script.hpp PS1ScriptCode + the big-value side table, Script.cpp) against
the bytecode files: tools/logic_test.sh dumps scriptCode.slot and s_ps1ScriptBig from the console; each slot is
decoded (a slot equal to -0x8000 takes its value from the table) and compared with GlobalCode + the stage's code as
LoadBytecode lays it out. Slots the scripts write at runtime (constant-indexed locals, tables written by
SetTableValue, tools/bytecode_scan.py) may differ; any other difference is an error.
Usage: script16_check.py BYTECODE_DIR STAGE OUT_PREFIX"""
import os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bytecode_scan as bs  # noqa: E402

bc, stage, out = sys.argv[1], sys.argv[2], sys.argv[3]
code, starts = bs.sub_starts(os.path.join(bc, stage + '.bin'))
# The global objects' subs are in GlobalCode.bin's object table (their code sits first in scriptCode when the stage
# loads the globals, so their offsets are the same).
if len(code) > len(bs.load(os.path.join(bc, stage + '.bin'))[0]):
    gsubs = bs.load(os.path.join(bc, 'GlobalCode.bin'))[2]
    starts += [x for t in gsubs for x in t if 0 <= x < len(code)]
slots = open(out + '_code.bin', 'rb').read()
slots = struct.unpack('<%dh' % (len(slots) // 2), slots)[:len(code)]
big = open(out + '_big.bin', 'rb').read()
big = dict(struct.unpack_from('<ii', big, 8 * i) for i in range(len(big) // 8))

# slots the scripts may write
funcs, vars_ = bs.tables(3)
local = vars_.index('VAR_LOCAL')
writable = set()
for s in starts:
    p, depth = s, 0
    while True:
        op = code[p]; p += 1
        name, size = funcs[op]
        ops = []
        for _ in range(size):
            t = code[p]; p += 1
            if t == 1:
                arr = code[p]; p += 1
                idx = None
                if arr in (1, 2, 3):
                    idx = (code[p], code[p + 1]); p += 2
                ops.append(('var', code[p], idx)); p += 1
            elif t == 2:
                ops.append(('int', code[p])); p += 1
            elif t == 3:
                p += code[p] // 4 + 2
                ops.append(('str',))
        for o in ops:
            if o[0] == 'var' and o[1] == local and o[2] and o[2][0] == 0:
                writable.add(o[2][1])
        if name == 'SetTableValue' and ops[2][0] == 'int':
            t = ops[2][1]
            writable.update(range(t, t + code[t] + 1))
        depth += (name in bs.OPEN) - (name in bs.CLOSE)
        if name in ('End', 'EndFunction') or (name == 'return' and depth <= 0):
            break

bad = runtime = nbig = 0
for i, want in enumerate(code):
    v = slots[i]
    if v == -0x8000:
        nbig += 1
        v = big.get(i, None)
    if v != want:
        if i in writable:
            runtime += 1
        else:
            bad += 1
            if bad <= 5:
                print('slot %d: file %d, ps1 %r' % (i, want, v))
print('%s: %d script ints, %d stored big; %d differ at runtime-written slots, %d differ elsewhere'
      % (stage, len(code), nbig, runtime, bad))
sys.exit(1 if bad else 0)
