#!/usr/bin/env python3
"""Exact A/B check of two tools/perf_scene.sh SHOTS= runs (docs/perf-43-tracker.md): for every frame both runs saved,
compare both framebuffers (VRAM 0,0 and 0,256, 320 x 240) and the entity table. Exit 1 on any difference.
Usage: shots_cmp.py REF_OUT NEW_OUT"""
import glob, os, re, sys
ref, new = sys.argv[1], sys.argv[2]
frames = sorted(int(re.search(r'_shot_(\d+)\.vram$', p).group(1)) for p in glob.glob(ref + '_shot_*.vram'))
bad = 0
def fb(raw, y0):
    return b''.join(raw[(y * 1024) * 2:(y * 1024 + 320) * 2] for y in range(y0, y0 + 240))
for f in frames:
    a, b = ref + '_shot_%d.vram' % f, new + '_shot_%d.vram' % f
    if not os.path.exists(b):
        print('frame %d: missing in %s' % (f, new)); bad += 1; continue
    ra, rb = open(a, 'rb').read(), open(b, 'rb').read()
    msg = []
    for i, y0 in enumerate((0, 256)):
        da, db = fb(ra, y0), fb(rb, y0)
        if da != db:
            n = sum(1 for k in range(0, len(da), 2) if da[k:k + 2] != db[k:k + 2])
            msg.append('fb%d %d px differ' % (i, n))
    ea, eb = ref + '_ent_%d.bin' % f, new + '_ent_%d.bin' % f
    if os.path.exists(ea) and os.path.exists(eb) and open(ea, 'rb').read() != open(eb, 'rb').read():
        msg.append('entities differ')
    print('frame %d: %s' % (f, ', '.join(msg) if msg else 'identical'))
    bad += bool(msg)
print('SHOTS %d frames, %d differ' % (len(frames), bad))
sys.exit(1 if bad or not frames else 0)
