#!/usr/bin/env python3
"""Sonic 2 (RSDKv4) PS1 feasibility report (docs/30 phase 1.3): SPU RAM, disc size, script/stage RAM.
Ported from the Sonic CD tool. (VRAM for sprites: tools/atlas/static_manifest.py + build_atlas.py --report.)

- SPU RAM: every stage's global (GameConfig) + stage (StageConfig) SFX as SPU-ADPCM (16 bytes per 28 samples,
  mono) at 44.1 / 32 / 22.05 / 18.9 kHz, against the 520,160 B the Sonic CD port has free for samples.
- Disc: music as 37.8 kHz 4-bit stereo CD-XA, 8 channels interleaved at 2x (tracks packed longest-first into
  the least-filled channel; 150 sectors/s); data = the non-source files of the Data tree + Bytecode.
- RAM: bytecode ints per stage (GlobalCode + the stage's file) vs SCRIPTCODE/JUMPTABLE, act entity counts vs
  ENTITY_COUNT (+ the highest object.value index any script uses: v4 entities carry 48), layer sizes.

Usage: feasibility.py DATA_DIR
"""
import glob, os, re, struct, subprocess, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'atlas'))
import bytecode_scan as bs  # noqa: E402
import object_sheets as osh  # noqa: E402

SPU_FREE = 0x7FFE0 - 0x1000
RATES = (44100, 32000, 22050, 18900)
SECTOR = 2352
ENTITY_BYTES = 5 * 4 + 48 * 4 + 9 * 4 + 2 + 30   # RSDKv4/Object.hpp struct Entity (floorSensors[5])


def samples(path):
    out = subprocess.run(['ffprobe', '-v', 'error', '-select_streams', 'a:0', '-show_entries',
                          'stream=sample_rate,duration:format=duration', '-of', 'default=nw=1', path],
                         capture_output=True, text=True).stdout
    kv = dict(l.split('=', 1) for l in out.split() if '=' in l)
    dur = float(kv.get('duration', 'N/A') if kv.get('duration', 'N/A') != 'N/A' else 0)
    return dur, int(kv.get('sample_rate', 44100))


def adpcm_bytes(dur, rate):
    return (int(dur * rate) + 27) // 28 * 16 + 16


def act_info(path):
    """(width, height in chunks, entity count) of an Act*.bin (RSDKv4/Scene.cpp LoadActLayout)."""
    d = open(path, 'rb').read()
    p = 1 + d[0] + 4 + 1
    w, h = d[p], d[p + 2]
    p += 4 + w * h * 2
    return w, h, struct.unpack_from('<H', d, p)[0]


def bg_layers(path):
    out = []
    if not os.path.exists(path):
        return out
    d, p = open(path, 'rb').read(), 0
    n = d[p]; p += 1
    for _ in range(2):
        c = d[p]; p += 1 + 4 * c
    for _ in range(n):
        xs, ys = d[p], d[p + 2]
        p += 8
        while True:
            b = d[p]; p += 1
            if b == 0xFF:
                b2 = d[p]; p += 1
                if b2 == 0xFF:
                    break
                p += 1
        p += xs * ys * 2
        out.append((xs, ys))
    return out


def blocks_len(path):
    d = open(path, 'rb').read()
    a, p = bs.blocks(d, 0)
    b, _ = bs.blocks(d, p)
    return len(a), len(b)


def max_value_index(bcdir):
    funcs, vars_ = bs.tables(3)
    idx = {i: int(m.group(1)) for i, v in enumerate(vars_) for m in [re.match(r'VAR_OBJECTVALUE(\d+)$', v)] if m}
    best = -1
    for f in glob.glob(os.path.join(bcdir, '*.bin')):
        code, starts = bs.sub_starts(f)
        for s in starts:
            for _, n, ops in bs.walk(code, s, funcs, len(vars_)):
                for k, v in ops:
                    if k == 'var' and v in idx:
                        best = max(best, idx[v])
    return best


def main():
    data = sys.argv[1]
    bcdir = os.path.join(os.path.dirname(os.path.normpath(data)), 'Bytecode')
    _, gsfx, players, lists = osh.game_config(data)
    folders = []
    for lst in lists:
        for f, _, _ in lst:
            if f not in folders and os.path.exists(os.path.join(data, 'Stages', f, 'StageConfig.bin')):
                folders.append(f)
    cache = {}

    def need(paths, rt):
        tot = 0
        for s in paths:
            if s not in cache:
                cache[s] = samples(os.path.join(data, 'SoundFX', s))
            tot += adpcm_bytes(cache[s][0], rt)
        return tot

    print('## SPU RAM (sample area %d B)' % SPU_FREE)
    print('global set: %d SFX, %s' % (len(gsfx), ', '.join('%d Hz %d B' % (rt, need(gsfx, rt)) for rt in RATES)))
    for f in folders:
        glob_, ssfx, _ = osh.stage_config(data, f)
        row = []
        for rt in RATES:
            row.append((need(gsfx, rt) if glob_ else 0) + need(ssfx, rt))
        fit = next((rt for rt, n in zip(RATES, row) if n <= SPU_FREE), None)
        print('  %-9s stage SFX %2d | %s | best uniform rate %s' % (
            f, len(ssfx), ' '.join('%6d' % n for n in row), fit or 'NONE FITS'))
    rates = sorted({cache[s][1] for s in cache})
    print('source rates: %s' % rates)

    print('\n## Disc')
    music = sorted(glob.glob(os.path.join(data, 'Music', '**', '*.ogg'), recursive=True))
    durs = sorted((samples(m)[0] for m in music), reverse=True)
    chans = [0.0] * 8
    for d in durs:
        chans[chans.index(min(chans))] += d
    xa = int(max(chans) * 150) + 8 * len(durs)
    print('music: %d tracks, %.1f min; one 8-channel XA file: longest channel %.1f min -> %d sectors = %.0f MB'
          % (len(durs), sum(durs) / 60, max(chans) / 60, xa, xa * SECTOR / 1e6))
    src = ('.ogg', '.wav', '.gif')
    dsize = sum(os.path.getsize(p) for p in glob.glob(os.path.join(data, '**', '*'), recursive=True)
                if os.path.isfile(p) and not p.endswith(src))
    bsize = sum(os.path.getsize(p) for p in glob.glob(os.path.join(bcdir, '*.bin')))
    print('data (non-source files) %.1f MB + bytecode %.1f MB + converted atlases/strips/SFX (~30 MB, as Sonic CD)'
          ' -> disc ~%.0f MB of 650' % (dsize / 1e6, bsize / 1e6, (xa * SECTOR + dsize + bsize + 30e6) / 1e6))
    vids = glob.glob(os.path.join(data, '**', '*.ogv'), recursive=True) + glob.glob(os.path.join(data, '..', 'videos', '*'))
    print('video files: %d' % len(vids))

    print('\n## RAM')
    g = blocks_len(os.path.join(bcdir, 'GlobalCode.bin'))
    rows = []
    for f in folders:
        glob_, _, _ = osh.stage_config(data, f)
        sc, jt = blocks_len(os.path.join(bcdir, f + '.bin'))
        acts = [act_info(a) for a in sorted(glob.glob(os.path.join(data, 'Stages', f, 'Act*.bin')))]
        bgs = bg_layers(os.path.join(data, 'Stages', f, 'Backgrounds.bin'))
        rows.append((f, sc + (g[0] if glob_ else 0), jt + (g[1] if glob_ else 0), max((a[2] for a in acts), default=0),
                     max((a[0] for a in acts), default=0), max((a[1] for a in acts), default=0), bgs))
    for r in rows:
        print('  %-9s script %6d ints (%4d KB) jump %5d | entities max %4d | FG max %3dx%-3d chunks | BG %d layers %s' % (
            r[0], r[1], r[1] * 4 // 1024, r[2], r[3], r[4], r[5], len(r[6]),
            ' '.join('%dx%d' % l for l in r[6])))
    mx = max(rows, key=lambda r: r[1])
    me = max(rows, key=lambda r: r[3])
    vi = max_value_index(bcdir)
    print('max script %d ints = %d KB (%s) vs SCRIPTCODE_COUNT 0x40000 (1 MB); max jump %d vs 0x4000'
          % (mx[1], mx[1] * 4 // 1024, mx[0], max(r[2] for r in rows)))
    print('entities: max %d in an act (%s) + 32 reserved + 128 temp = %d vs ENTITY_COUNT 0x4A0; Entity %d B -> '
          'full array %d KB, sized to the data %d KB' % (me[3], me[0], me[3] + 160, ENTITY_BYTES,
                                                         0x4A0 * ENTITY_BYTES // 1024, (me[3] + 160) * ENTITY_BYTES // 1024))
    print('highest object.value index used by any script: %d (of 48)' % vi)


if __name__ == '__main__':
    main()
