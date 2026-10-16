#!/usr/bin/env python3
"""GPU time of one frame's DMA chain (docs/43; from RSDKv5-ps1/tools/play/gpu_cost.py, docs/32 9f), priced with
DuckStation's model (duckstation/src/core/gpu.cpp: AddDrawTriangleTicks, AddDrawRectangleTicks, UpdateCLUTIfNeeded,
the fill and VRAM copy costs; GPU ticks, 2 per CPU cycle; one NTSC hblank = 2152 cycles). psx-spx gives no GPU
drawing times (dmachannels.md "DMA Transfer Rates"), so this is a model, not a measurement. The chain is walked from
its head in a 2 MB RAM dump taken where the frame's chain is complete, as psyqo sends it at the next flip (tools/perf_gpu.sh).

Usage: gpu_cost.py RAM.bin HEAD [TAIL TAILCOUNT HEADOVERSIZED] [--base 0x80000000] [--top N]
The chain is walked as psyqo's sendChain() will send it (nugget/psyqo/src/gpu.cpp): the end marker is only written
into the tail node at send time, so the walk stops at psyqo's m_chainTail (TAIL, its size word TAILCOUNT =
m_chainTailCount); link words carry flags (bit 23: a big node, bit 0: the next node is oversized, its size >> 4),
and the head's oversized flag is m_chainHeadOversized.
Prints the frame's total (ticks, hblanks), the split per primitive kind (and per CLUT reload), the N most expensive
primitives, and a SUM line: GPU hblanks, pixels drawn (fills apart), overdraw (those pixels / 320 x 240),
primitives, chain words."""
import struct, sys
from collections import defaultdict

HBL_CYCLES = 2152

args = [a for a in sys.argv[1:] if not a.startswith('--')]
opts = sys.argv[1:]
ram = open(args[0], 'rb').read()
head = int(args[1], 0)
base = int(opts[opts.index('--base') + 1], 0) if '--base' in opts else 0x80000000
top = int(opts[opts.index('--top') + 1]) if '--top' in opts else 15


def word(addr):
    off = (addr - base) & 0x7FFFFF
    return struct.unpack_from('<I', ram, off)[0] if off + 4 <= len(ram) else 0


tail = int(args[2], 0) & 0x7FFFFC if len(args) > 2 else None
tail_count = (int(args[3], 0) >> 24) if len(args) > 3 else None
oversized = len(args) > 4 and int(args[4], 0) != 0

# walk the linked list: header = count << 24 | flags | next (0xFFFFFF = end)
words = []
node, nodes, seen = head & 0x7FFFFC, 0, set()
while nodes < 200000:
    if node in seen:
        print('chain loops at %06x' % node)
        break
    seen.add(node)
    h = word(0x80000000 | node)
    cnt = h >> 24
    last = tail is not None and node == tail
    if last:
        cnt = tail_count
    if oversized:
        cnt <<= 4
    for i in range(cnt):
        words.append(word(0x80000000 | node + 4 + 4 * i))
    nodes += 1
    nxt = h & 0xFFFFFF
    if last or nxt == 0xFFFFFF:
        break
    oversized = bool(nxt & 0x800000) and bool(nxt & 1)
    node = nxt & 0x7FFFFC
if tail is not None and node != tail:
    print('warning: the walk ended at %06x, not at the tail %06x' % (node, tail))

# GP0 state
tex_mode = 0          # E1 bits 7-8: 0 = 4-bit, 1 = 8-bit, 2 = 15-bit
semi_mode = 0
check_mask = False
area = (0, 0, 1023, 511)
offset = (0, 0)
clut_reg, clut_8 = None, False

cost = defaultdict(int)     # kind -> ticks
count = defaultdict(int)
pixels = defaultdict(int)
items = []                  # (ticks, kind, description)


def clamp_rect(x, y, w, h):
    x0, y0 = max(x, area[0]), max(y, area[1])
    x1, y1 = min(x + w, area[2] + 1), min(y + h, area[3] + 1)
    return max(0, x1 - x0), max(0, y1 - y0)


def tri_px(a, b, c):
    clampx = lambda v: (min(max(v[0], area[0]), area[2]), min(max(v[1], area[1]), area[3]))
    a, b, c = clampx(a), clampx(b), clampx(c)
    return abs((a[0] * b[1] + b[0] * c[1] + c[0] * a[1] - a[0] * c[1] - b[0] * a[1] - c[0] * b[1]) // 2)


def s16(v):
    v &= 0x7FF
    return v - 0x800 if v & 0x400 else v


def xy(w):
    return (s16(w) + offset[0], s16(w >> 16) + offset[1])


def clut_reload(c, eight):
    global clut_reg, clut_8
    if c != clut_reg or (eight and not clut_8):
        clut_reg, clut_8 = c, eight
        t = 256 if eight else 16
        cost['clut reload 8-bit' if eight else 'clut reload 4-bit'] += t
        count['clut reload 8-bit' if eight else 'clut reload 4-bit'] += 1


i = 0
n = len(words)
while i < n:
    w0 = words[i]
    op = w0 >> 24
    if op == 0x00 or op == 0x01 or (0x03 <= op <= 0x1F):
        i += 1
        continue
    if op == 0x02:  # fill
        fw, fh = words[i + 2] & 0x3FF, (words[i + 2] >> 16) & 0x1FF
        fw = ((fw + 0xF) & ~0xF)
        t = 46 + ((fw // 8) + 9) * fh
        cost['fill'] += t; count['fill'] += 1; pixels['fill'] += fw * fh
        i += 3
        continue
    if 0x20 <= op <= 0x3F:  # polygon
        quad, textured, shaded, semi = bool(op & 8), bool(op & 4), bool(op & 0x10), bool(op & 2)
        nv = 4 if quad else 3
        # flat: the command / colour word, then (xy [uv]) per vertex; shaded: (colour xy [uv]) per vertex
        wpv = (2 if shaded else 1) + textured
        total = nv * wpv + (0 if shaded else 1)
        verts, uvs = [], []
        for k in range(nv):
            vbase = i + 1 + k * wpv if not shaded else i + k * wpv + 1
            verts.append(xy(words[vbase]))
            if textured:
                uvs.append(words[vbase + 1])
        setup = [[46, 226], [334, 496]] if not quad else [[82, 262], [370, 532]]
        t = setup[shaded][textured]
        if textured:
            tp = (uvs[1] >> 16) & 0xFFFF
            mode = (tp >> 7) & 3
            if mode < 2:
                clut_reload((uvs[0] >> 16) & 0xFFFF, mode == 1)
        px = tri_px(verts[0], verts[1], verts[2]) + (tri_px(verts[1], verts[2], verts[3]) if quad else 0)
        f = px * (2 if textured else 1)
        if semi or check_mask:
            f += (f + 1) // 2
        t += f
        kind = '%s%s%s%s' % ('quad' if quad else 'tri', ' textured' if textured else '', ' shaded' if shaded else '', ' semi' if semi else '')
        cost[kind] += t; count[kind] += 1; pixels[kind] += px
        items.append((t, kind, 'verts %s' % verts))
        i += total
        continue
    if 0x40 <= op <= 0x5F:  # lines (polyline ends with 0x5xxx5xxx)
        shaded, poly = bool(op & 0x10), bool(op & 8)
        if not poly:
            ln = 4 if shaded else 3
            a = xy(words[i + 1]); b = xy(words[i + (3 if shaded else 2)])
            t = max(abs(a[0] - b[0]), abs(a[1] - b[1]))
            cost['line'] += t; count['line'] += 1
            i += ln
        else:
            j = i + 1
            while j < n and (words[j] & 0xF000F000) != 0x50005000:
                j += 1
            cost['polyline'] += 2 * (j - i); count['polyline'] += 1
            i = j + 1
        continue
    if 0x60 <= op <= 0x7F:  # rectangles
        textured, semi = bool(op & 4), bool(op & 2)
        size = (op >> 3) & 3
        x, y = xy(words[i + 1])
        k = i + 2
        if textured:
            clutw = words[k] >> 16
            k += 1
        if size == 0:
            rw, rh = words[k] & 0x3FF, (words[k] >> 16) & 0x1FF
            k += 1
        else:
            rw = rh = {1: 1, 2: 8, 3: 16}[size]
        dw, dh = clamp_rect(x, y, rw, rh)
        per = dw
        if textured:
            if tex_mode == 0:
                per += dw
            elif tex_mode == 1:
                if dw > 128:
                    per += (dw // 4) * 8
                elif dw * dh > 2048:
                    per += (dw // 4) * (4 * (128 // max(dw, 1)))
                else:
                    per += dw
            else:
                if dw > 128:
                    per += (dw // 2) * 8
                elif dw * dh > 1024:
                    per += (dw // 4) * (8 * (128 // max(dw, 1)))
                else:
                    per += dw
            if tex_mode < 2:
                clut_reload(clutw, tex_mode == 1)
        if semi or check_mask:
            per += (dw + 1) // 2
        t = 16 + per * dh
        kind = 'rect%s%s%s' % (' textured %s' % ['4-bit', '8-bit', '15-bit', '15-bit'][tex_mode] if textured else ' flat',
                               ' semi' if semi else '', ' wide' if textured and dw > 128 else '')
        cost[kind] += t; count[kind] += 1; pixels[kind] += dw * dh
        items.append((t, kind, '%dx%d at %d,%d' % (rw, rh, x, y)))
        i = k
        continue
    if op == 0x80:  # VRAM copy
        cw, ch = words[i + 3] & 0xFFFF, words[i + 3] >> 16
        t = cw * ch * 2
        cost['vram copy'] += t; count['vram copy'] += 1
        i += 4
        continue
    if op == 0xA0:  # CPU -> VRAM
        cw, ch = words[i + 2] & 0xFFFF, words[i + 2] >> 16
        nw = (cw * ch + 1) // 2
        cost['cpu->vram'] += nw; count['cpu->vram'] += 1
        i += 3 + nw
        continue
    if op == 0xC0:
        i += 3
        continue
    if op == 0xE1:
        tex_mode, semi_mode = (w0 >> 7) & 3, (w0 >> 5) & 3
    elif op == 0xE3:
        area = (w0 & 0x3FF, (w0 >> 10) & 0x1FF, area[2], area[3])
    elif op == 0xE4:
        area = (area[0], area[1], w0 & 0x3FF, (w0 >> 10) & 0x1FF)
    elif op == 0xE5:
        offset = (s16(w0), s16(w0 >> 11))
    elif op == 0xE6:
        check_mask = bool(w0 & 2)
    i += 1

total = sum(cost.values())
print('chain: %d nodes, %d words; GPU %d ticks = %d cycles = %.1f hblanks (DuckStation model)' % (
    nodes, len(words), total, total // 2, total / 2 / HBL_CYCLES))
for k, v in sorted(cost.items(), key=lambda kv: -kv[1]):
    print('  %-34s %6.1f hbl  %5d prims  %7d px' % (k, v / 2 / HBL_CYCLES, count[k], pixels[k]))
drawn = sum(v for k, v in pixels.items() if k != 'fill')
prims = sum(v for k, v in count.items() if not k.startswith('clut'))
print('SUM gpu_hbl=%.1f px=%d overdraw=%.2f fill_px=%d prims=%d words=%d' % (
    total / 2 / HBL_CYCLES, drawn, drawn / 76800.0, pixels.get('fill', 0), prims, len(words)))
print('most expensive:')
for t, k, d in sorted(items, reverse=True)[:top]:
    print('  %6.2f hbl  %-28s %s' % (t / 2 / HBL_CYCLES, k, d))
