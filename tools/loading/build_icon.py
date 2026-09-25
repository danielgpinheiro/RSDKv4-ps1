#!/usr/bin/env python3
"""Build-time loading icon: an animated GIF -> ps1/loading_icon.h (drawn by ps1/loading.cpp).

Sonic 2 port: PS1/Memcard icons/sonic-2-loading.gif (user's choice, 2026-09-25): 8 frames of 576x144,
each a strip of four 144x144 Sonic poses; the first cell (CROP) runs through 8 distinct poses, used at
the GIF's 30 ms. Sonic CD used sonic-1-loading.gif, the Nexus port sonic-spinning.gif (48x48).

Frames larger than SIZE x SIZE are reduced to fit, keeping the aspect ratio, by a *mode*
downsample: each icon pixel takes the most common colour of its source block (transparent if
that wins). The source is pixel art enlarged by a non-integer factor (~5.5x), so this keeps
edges crisp where filtering would blur them. Frames are centred in SIZE x SIZE cells.

Output: one 4-bit texture (PER_ROW frames per SIZE-row band, 256 texels wide) + a 16-entry CLUT
(index 0 = transparent = 0x0000; visible black -> 0x8000). Every frame is decoded back and
compared with the reduced frames (load_frames(), shared with check_icon.py).

Usage: build_icon.py [GIF] [OUT_HEADER] [--frame-ms N] [--crop X,Y,W,H]
  defaults: ../Memcard icons/sonic-2-loading.gif cropped to its first 144x144 cell, ps1/loading_icon.h;
  frame time from the GIF, or --frame-ms (default 83 ms when the GIF has none)
"""
import collections, os, sys
from PIL import Image, ImageSequence

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..'))
SIZE = 48
PER_ROW = 5
TEX_W = 256  # texels
DEFAULT_FRAME_MS = 83
DEFAULT_CROP = (0, 0, 144, 144)  # the default GIF's first pose cell


def default_gif():
    p = os.path.join(REPO, '..', 'Memcard icons', 'sonic-2-loading.gif')
    if not os.path.exists(p):
        sys.exit('sonic-2-loading.gif not found in PS1/Memcard icons/')
    return p


def conv(rgb):
    r, g, b = rgb
    v = (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10)
    return v if v else 0x8000


def mode_down(f, tw, th):
    w, h = f.size
    px = f.load()
    out = Image.new('RGBA', (tw, th), (0, 0, 0, 0))
    for ty in range(th):
        for tx in range(tw):
            x0, x1 = tx * w // tw, max((tx + 1) * w // tw, tx * w // tw + 1)
            y0, y1 = ty * h // th, max((ty + 1) * h // th, ty * h // th + 1)
            c = collections.Counter()
            for y in range(y0, y1):
                for x in range(x0, x1):
                    p = px[x, y]
                    c[p[:3] if p[3] >= 128 else None] += 1
            k = c.most_common(1)[0][0]
            if k is not None:
                out.putpixel((tx, ty), k + (255,))
    return out


def load_frames(gif, crop=DEFAULT_CROP):
    """The GIF's frames (cropped to crop = (x, y, w, h), None = whole) as SIZE x SIZE RGBA images (reduced
    + centred) and the frame time (ms)."""
    im = Image.open(gif)
    src = [f.convert('RGBA') for f in ImageSequence.Iterator(im)]
    if crop:
        x, y, w, h = crop
        src = [f.crop((x, y, x + w, y + h)) for f in src]
    ms = [f.info.get('duration', 0) for f in ImageSequence.Iterator(Image.open(gif))]
    w, h = src[0].size
    scale = min(1.0, SIZE / w, SIZE / h)
    tw, th = max(1, round(w * scale)), max(1, round(h * scale))
    frames = []
    for f in src:
        small = f if (tw, th) == (w, h) else mode_down(f, tw, th)
        cell = Image.new('RGBA', (SIZE, SIZE), (0, 0, 0, 0))
        cell.paste(small, ((SIZE - tw) // 2, (SIZE - th) // 2))
        frames.append(cell)
    # More than 15 colours (4-bit CLUT + transparent): the least used ones join their nearest kept colour
    # (squared RGB distance), deterministically; the frames returned already carry the merged colours.
    use = collections.Counter(p[:3] for f in frames for p in f.getdata() if p[3] >= 128)
    if len(use) > 15:
        keep = [c for c, _ in sorted(use.items(), key=lambda kv: (-kv[1], kv[0]))[:15]]
        near = {c: min(keep, key=lambda k: (sum((a - b) ** 2 for a, b in zip(c, k)), k)) for c in use}
        for f in frames:
            f.putdata([near[p[:3]] + (255,) if p[3] >= 128 else (0, 0, 0, 0) for p in f.getdata()])
    return frames, (ms[0] if ms and ms[0] else None)


def main():
    args = [a for a in sys.argv[1:]]
    frame_ms = None
    crop = DEFAULT_CROP
    if '--frame-ms' in args:
        i = args.index('--frame-ms'); frame_ms = int(args[i + 1]); del args[i:i + 2]
    if '--crop' in args:
        i = args.index('--crop'); crop = tuple(int(v) for v in args[i + 1].split(',')); del args[i:i + 2]
    gif = args[0] if len(args) > 0 else default_gif()
    out = args[1] if len(args) > 1 else os.path.join(REPO, 'ps1', 'loading_icon.h')
    frames, gif_ms = load_frames(gif, crop)
    frame_ms = frame_ms or gif_ms or DEFAULT_FRAME_MS
    rows = (len(frames) + PER_ROW - 1) // PER_ROW
    tex_h = rows * SIZE
    colours = []
    for f in frames:
        for y in range(SIZE):
            for x in range(SIZE):
                p = f.getpixel((x, y))
                if p[3] >= 128 and p[:3] not in colours:
                    colours.append(p[:3])
    if len(colours) > 15:
        sys.exit('ERROR: %d colours > 15 (4-bit CLUT)' % len(colours))
    index = {c: i + 1 for i, c in enumerate(colours)}
    clut = [0x0000] + [conv(c) for c in colours] + [0x0000] * (15 - len(colours))
    tex = [[0] * TEX_W for _ in range(tex_h)]
    for n, f in enumerate(frames):
        ox, oy = (n % PER_ROW) * SIZE, (n // PER_ROW) * SIZE
        for y in range(SIZE):
            for x in range(SIZE):
                p = f.getpixel((x, y))
                tex[oy + y][ox + x] = index[p[:3]] if p[3] >= 128 else 0
    words = []
    for y in range(tex_h):
        for x in range(0, TEX_W, 4):
            t = tex[y][x:x + 4]
            words.append(t[0] | (t[1] << 4) | (t[2] << 8) | (t[3] << 12))
    for n, f in enumerate(frames):  # self-check
        ox, oy = (n % PER_ROW) * SIZE, (n // PER_ROW) * SIZE
        for y in range(SIZE):
            for x in range(SIZE):
                w = words[(oy + y) * (TEX_W // 4) + (ox + x) // 4]
                i = (w >> (((ox + x) & 3) * 4)) & 0xF
                p = f.getpixel((x, y))
                assert clut[i] == (conv(p[:3]) if p[3] >= 128 else 0x0000), 'frame %d (%d,%d)' % (n, x, y)
    name = os.path.basename(gif)
    lines = ['// Generated by tools/loading/build_icon.py from %s -- do not edit.' % name,
             '// 4-bit texture: %d frames of %dx%d (%d per %d-row band), %dx%d texels = %dx%d halfwords.' % (
                 len(frames), SIZE, SIZE, PER_ROW, SIZE, TEX_W, tex_h, TEX_W // 4, tex_h),
             '#pragma once', '#include <stdint.h>', '',
             '#define PS1_LOADING_ICON_FRAMES   %d' % len(frames),
             '#define PS1_LOADING_ICON_SIZE     %d' % SIZE,
             '#define PS1_LOADING_ICON_PER_ROW  %d' % PER_ROW,
             '#define PS1_LOADING_ICON_TEX_HW   %d // texture width in halfwords' % (TEX_W // 4),
             '#define PS1_LOADING_ICON_TEX_H    %d' % tex_h,
             '#define PS1_LOADING_ICON_FRAME_US %d // frame time' % (frame_ms * 1000), '',
             'alignas(4) static const uint16_t s_ps1LoadingIconClut[16] = {',
             '    ' + ', '.join('0x%04x' % v for v in clut) + ',', '};',
             'alignas(4) static const uint16_t s_ps1LoadingIconTex[%d] = {' % len(words)]
    for i in range(0, len(words), 16):
        lines.append('    ' + ', '.join('0x%04x' % v for v in words[i:i + 16]) + ',')
    lines.append('};')
    open(out, 'w').write('\n'.join(lines) + '\n')
    print('%s: %s, %d frames -> %dx%d cells, %d colours + transparent, %d ms/frame, texture %d B, self-check exact' % (
        os.path.relpath(out, REPO), name, len(frames), SIZE, SIZE, len(colours), frame_ms, len(words) * 2))


if __name__ == '__main__':
    main()
