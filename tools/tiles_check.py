#!/usr/bin/env python3
"""Check the PS1 chunk tiles (tools/logic_test.sh dumps of tiles128x128: the packed index/direction/visualPlane
ushorts and the nibble-packed collision flags, RSDKv4/Scene.hpp) against the stage's 128x128Tiles.bin, decoded as
RSDKv4/Scene.cpp LoadStageChunks does.
Usage: tiles_check.py DATA_STAGE_DIR OUT_PREFIX"""
import struct, sys
stage, out = sys.argv[1], sys.argv[2]
d = open(stage + '/128x128Tiles.bin', 'rb').read()
tile = struct.unpack('<32768H', open(out + '_tile.bin', 'rb').read())
tcol = open(out + '_tcol.bin', 'rb').read()
bad = 0
for i in range(0x8000):
    e0, e1, e2 = d[3 * i], d[3 * i + 1], d[3 * i + 2]
    e0 &= 0x3F
    plane, e0 = e0 >> 4, e0 & 0xF
    direction, e0 = e0 >> 2, e0 & 3
    want = (e1 + (e0 << 8), direction, plane, e2 >> 4, e2 & 0xF)
    got = (tile[i] & 0x3FF, (tile[i] >> 10) & 3, tile[i] >> 12, tcol[i] & 0xF, tcol[i] >> 4)
    if want != got:
        bad += 1
        if bad <= 5:
            print('tile', i, 'file', want, 'ps1', got)
print('chunk tiles: %d of 32768 differ' % bad)
sys.exit(1 if bad else 0)
