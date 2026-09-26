#!/usr/bin/python3
"""Check a stage's VRAM residents against the build-time files (phase 3.1).

  tile pages : Data/Stages/<Stage>/16x16Tiles.vram pages vs VRAM X 576-831 (tileset.hh layout)
  atlas      : every block of Data/Sprites/Atlas/<Stage>.atl vs its VRAM rectangle
  palette    : optional PALETTE.bin (GDB dump of activePalette32, 256 x {r,g,b}) entries 0x80-0xFF vs
               the tile GIF's colour table (the .vram header)
Usage: check_vram.py VRAM.bin DATA_DIR STAGE [PALETTE.bin]   (STAGE may carry the atlas suffix: Zone01_t, _k, _st)
Copied from RSDKv3-ps1 (Sonic CD) for the RSDKv4 (Sonic 2) port: four player variants; no BG strips yet (phase 3.5).
"""
import struct, sys
import numpy as np


def main():
    vram_path, data, stage = sys.argv[1:4]
    vram = np.frombuffer(open(vram_path, 'rb').read(), '<u2').reshape(512, 1024)
    ok = True
    atlas = stage
    for sfx in ('_st', '_t', '_k'):  # player variants (build_atlas.py --suffix)
        if stage.endswith(sfx):
            stage = stage[:-len(sfx)]
            break
    sfx = atlas[len(stage):]
    t = open('%s/Stages/%s/16x16Tiles.vram' % (data, stage), 'rb').read()
    pal = t[4:4 + 768]
    pages = np.frombuffer(t[4 + 768:], '<u2').reshape(4, 256, 128)
    for p in range(4):
        x, y = (9 + 2 * (p & 1)) * 64, (p >> 1) * 256
        same = np.array_equal(vram[y:y + 256, x:x + 128], pages[p])
        ok &= same
        print('tile page %d at (%d,%d): %s' % (p, x, y, 'identical' if same else 'DIFFERENT'))
    a = open('%s/Sprites/Atlas/%s.atl' % (data, atlas), 'rb').read()
    sheets, clusters, cluts, blocks, fx, fy, off = struct.unpack_from('<6HI', a, 4)
    p = 20 + sheets * 56 + clusters * 16 + cluts * 20
    bad = 0
    for b in range(blocks):
        x, y, w, h, o = struct.unpack_from('<4HI', a, p + 12 * b)
        blk = np.frombuffer(a, '<u2', w * h, off + o).reshape(h, w)
        bad += not np.array_equal(vram[y:y + h, x:x + w], blk)
    ok &= bad == 0
    print('atlas: %d blocks, %d different' % (blocks, bad))
    import os
    pp = '%s/Sprites/Atlas/%s.pic' % (data, atlas)
    if os.path.exists(pp):  # on-demand pictures: which one each slot holds (whole picture identical)
        d = open(pp, 'rb').read()
        nslots, npics, nblk, _, doff = struct.unpack_from('<4HI', d, 4)
        slots = [struct.unpack_from('<4H2h', d, 16 + 12 * k) for k in range(nslots)]
        pics = [struct.unpack_from('<2H', d, 16 + 12 * nslots + 4 * i) for i in range(npics)]
        bo = 16 + 12 * nslots + 4 * npics
        blks = [struct.unpack_from('<4HI', d, bo + 12 * b) for b in range(nblk)]
        for k, (sx, sy, sw, sh, dx, dy) in enumerate(slots):
            held = [i for i, (fb, n) in enumerate(pics) if all(np.array_equal(
                vram[y + dy * 256:y + dy * 256 + h, x + dx * 64:x + dx * 64 + w],
                np.frombuffer(d, '<u2', w * h, doff + o).reshape(h, w)) for (x, y, w, h, o) in blks[fb:fb + n])]
            print('picture slot %d (%d,%d %dx%d): %s' % (k, sx, sy, sw, sh, 'picture %s identical' % held if held else 'no picture'))
    sp = '%s/Stages/%s/BGStrips%s.bin' % (data, stage, sfx)
    if os.path.exists(sp):  # BG line strips (tools/bgstrips): every piece vs its VRAM rectangle
        d = open(sp, 'rb').read()
        count, = struct.unpack_from('<H', d, 4)
        off, sbad, spieces = 8 + count * 64, 0, 0
        for i in range(count):
            e = d[8 + 64 * i:8 + 64 * (i + 1)]
            lines, npieces = struct.unpack_from('<H', e, 4)[0], e[8]
            for k in range(npieces):
                x, y, w = struct.unpack_from('<HHH', e, 10 + 6 * k)
                blk = np.frombuffer(d, '<u2', lines * w // 2, off).reshape(lines, w // 2)
                off += lines * w
                sbad += not np.array_equal(vram[y:y + lines, x:x + w // 2], blk)
                spieces += 1
        ok &= sbad == 0
        print('strips: %d regions, %d pieces, %d different' % (count, spieces, sbad))
    if len(sys.argv) > 4:
        got = open(sys.argv[4], 'rb').read()
        diff = [c for c in range(0x80, 0x100) if got[c * 3:c * 3 + 3] != pal[c * 3:c * 3 + 3]]
        ok &= not diff
        print('palette 0x80-0xFF vs tile GIF: %s' % ('identical' if not diff else 'DIFFERENT at %s' % diff[:8]))
    print('OK' if ok else 'FAIL')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
