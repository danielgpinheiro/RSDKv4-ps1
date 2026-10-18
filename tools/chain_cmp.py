#!/usr/bin/env python3
"""Exact A/B check of the frames' GPU chains between two tools/perf_scene.sh runs made with GPU="f ..." KEEP_RAM=1
(docs/43 C1): for every frame both runs dumped, walk the chain from psyqo's head as tools/gpu_cost.py does and compare
the payload words in the order the GPU reads them. Link words are left out (node addresses move with code size), so this
checks the primitives, their words and their draw order. Exit 1 on any difference.
Usage: chain_cmp.py REF_OUT NEW_OUT"""
import glob, os, re, struct, subprocess, sys
ref, new = sys.argv[1], sys.argv[2]
here = os.path.dirname(os.path.abspath(__file__))
def words(out, f):
    head = [l.split() for l in open(out + '.gdblog') if l.startswith('GPUHEAD %d ' % f)]
    if not head:
        return None
    h = head[0]
    dump = '%s_gpu_%d.words' % (out, f)
    subprocess.run([sys.executable, os.path.join(here, 'gpu_cost.py'), '%s_gpu_%d.ram' % (out, f), '0x' + h[2], '0x' + h[3],
                    '0x' + h[4], h[5], '--dump-words', dump], check=True, stdout=subprocess.DEVNULL)
    b = open(dump, 'rb').read()
    os.remove(dump)
    return struct.unpack('<%dI' % (len(b) // 4), b)
frames = sorted(int(re.search(r'_gpu_(\d+)\.ram$', p).group(1)) for p in glob.glob(ref + '_gpu_*.ram'))
bad = 0
for f in frames:
    a, b = words(ref, f), words(new, f)
    if a is None or b is None:
        print('frame %d: missing' % f); bad += 1; continue
    if a == b:
        print('frame %d: identical (%d words)' % (f, len(a)))
        continue
    k = next((i for i in range(min(len(a), len(b))) if a[i] != b[i]), min(len(a), len(b)))
    print('frame %d: %d vs %d words, first difference at word %d (%s vs %s)' % (
        f, len(a), len(b), k, '%08x' % a[k] if k < len(a) else '-', '%08x' % b[k] if k < len(b) else '-'))
    bad += 1
print('CHAIN %d frames, %d differ' % (len(frames), bad))
sys.exit(1 if bad or not frames else 0)
