#!/usr/bin/env python3
"""Phase 5b evidence: check a VRAM dump taken during a load (tools/vramshot.py *_vram.bin).

The displayed buffer (Y = 0 or 256) must hold GIF frame FRAME at (264, 184): opaque GIF pixels
equal the icon CLUT colour, transparent ones equal the saved background (hidden buffer at
(128, 0)). Also checks the texture + CLUT uploaded into the hidden buffer.
Usage: check_icon.py VRAM.bin FRAME DISPLAYED_Y   (frames: build_icon.load_frames of the icon GIF)
"""
import os, sys
from PIL import Image, ImageSequence
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_icon  # noqa: E402
from build_icon import conv, SIZE as FW, SIZE as FH, PER_ROW

HERE = os.path.dirname(os.path.abspath(__file__))
X, Y = 320 - FW - 8, 240 - FH - 8


def main():
    vram_path, frame, dy = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    d = open(vram_path, 'rb').read()
    px = lambda x, y: d[(y * 1024 + x) * 2] | (d[(y * 1024 + x) * 2 + 1] << 8)
    hid = 256 - dy
    frames, _ = build_icon.load_frames(build_icon.default_gif())  # the same reduced frames
    f = frames[frame].load()
    bad_op = bad_tr = n_op = n_tr = 0
    for y in range(FH):
        for x in range(FW):
            got = px(X + x, dy + Y + y)
            p = f[x, y]
            if p[3] >= 128:
                n_op += 1
                bad_op += got != conv(p[:3])
            else:
                n_tr += 1
                bad_tr += got != px(128 + x, hid + y)
    # texture/CLUT in the hidden buffer decode back to every frame
    tex_bad = 0
    tex_h = (len(frames) + PER_ROW - 1) // PER_ROW * FH  # the CLUT sits right below the texture
    clut = [px(64 + i, hid + tex_h) for i in range(16)]
    for n, fr in enumerate(frames):
        ox, oy, fp = (n % PER_ROW) * FW, (n // PER_ROW) * FH, fr.load()
        for y in range(FH):
            for x in range(FW):
                w = px((ox + x) // 4, hid + oy + y)
                c = clut[(w >> (((ox + x) & 3) * 4)) & 0xF]
                q = fp[x, y]
                tex_bad += c != (conv(q[:3]) if q[3] >= 128 else 0)
    print('%s frame %d buf Y%d: opaque %d/%d exact, transparent %d/%d = background, texture+CLUT mismatches %d' % (
        os.path.basename(vram_path), frame, dy, n_op - bad_op, n_op, n_tr - bad_tr, n_tr, tex_bad))
    return 0 if bad_op == bad_tr == tex_bad == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
