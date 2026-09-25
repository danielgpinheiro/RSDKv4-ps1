#!/usr/bin/env python3
"""Dump PCSX-Redux VRAM via the web server (-webserver) and write PNGs.

Usage: vramshot.py OUT_PREFIX [BASELINE_PNG]
Writes OUT_PREFIX_vram.bin, OUT_PREFIX_vram.png (1024x512), OUT_PREFIX_fb0.png (Y 0-239)
and OUT_PREFIX_fb1.png (Y 256-495). With BASELINE_PNG (a complete 320x240 frame, e.g.
tools/baseline/ssz_full.png), prints how many pixels of each framebuffer differ from it;
the buffer that holds the finished frame should be ~0 (the other may be mid-draw).
"""
import struct, sys, urllib.request, warnings
warnings.filterwarnings('ignore', category=DeprecationWarning)
from PIL import Image

W, H = 1024, 512


def to_rgb(v):
    return ((v & 31) << 3, ((v >> 5) & 31) << 3, ((v >> 10) & 31) << 3)


def main():
    out = sys.argv[1]
    raw = urllib.request.urlopen('http://localhost:8080/api/v1/gpu/vram/raw', timeout=10).read()
    assert len(raw) == W * H * 2, len(raw)
    open(out + '_vram.bin', 'wb').write(raw)
    px = struct.unpack('<%dH' % (W * H), raw)
    img = Image.new('RGB', (W, H))
    img.putdata([to_rgb(v) for v in px])
    img.save(out + '_vram.png')
    for i, y0 in enumerate((0, 256)):
        fb = img.crop((0, y0, 320, y0 + 240))
        fb.save('%s_fb%d.png' % (out, i))
        nonblack = sum(1 for p in fb.getdata() if p != (0, 0, 0))
        msg = 'fb%d nonblack=%d' % (i, nonblack)
        if len(sys.argv) > 2:
            try:
                base = Image.open(sys.argv[2]).convert('RGB')
                diff = sum(1 for a, b in zip(fb.getdata(), base.getdata()) if a != b)
                msg += ' diff_vs_baseline=%d' % diff
            except FileNotFoundError:
                pass
        print(msg)


if __name__ == '__main__':
    main()
