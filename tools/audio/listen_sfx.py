#!/usr/bin/env python3
"""Listening check for the SFX banks (docs/30 "Scene3D block + SFX cap"): original vs what the PS1 plays.

For one stage bank on the disc tree (build/iso/Data/Stages/<STAGE>/PS1Sfx.bin, or Data/Game/PS1SfxResident.bin /
PS1SfxMenu.bin with STAGE = Resident / Menu), each sound is written twice to build/listen/<STAGE>/:
  NN_<name>_orig.wav          the source (.ogg / .wav) decoded to mono 16-bit at its own rate (ffmpeg)
  NN_<name>_ps1_<rate>.wav    the bank's SPU-ADPCM data decoded back (psx-spx "SPU ADPCM Samples") at its bank rate
and, unless --no-play, played in pairs with afplay: original, then PS1. By default only the sounds whose rate is
below 44,100 Hz; --all plays every one, --only NAME [NAME...] picks some (case-insensitive substrings). The rate
without the 400 KB cap comes from build/sfx_rates.txt (tools/audio/build_sfx.py) when it lists the stage.
The SPU resamples with a 4-tap Gaussian interpolator (a mild low-pass); afplay's resampler is cleaner, so the PS1
files sound slightly brighter here than on the console: judge the rate loss, and confirm in PCSX-Redux.

Usage: listen_sfx.py STAGE [--all | --only NAME...] [--no-play] [--src ../Sonic2_Extracted]
       (e.g. listen_sfx.py Zone01; listen_sfx.py Zone08 --only ring jump spring)
"""
import argparse, array, os, struct, subprocess, sys, tempfile, wave

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
import sfx_sets  # noqa: E402

FILTERS = ((0, 0), (60, 0), (115, -52), (98, -55), (122, -60))


def spu_decode(data):
    """SPU-ADPCM blocks -> 16-bit samples (psx-spx: shift / filter nibbles, 28 4-bit samples per 16-byte block)."""
    out, s1, s2 = array.array('h'), 0, 0
    for b in range(0, len(data) - 15, 16):
        head, flags = data[b], data[b + 1]
        shift, (f0, f1) = head & 0x0F, FILTERS[min(head >> 4, 4)]
        for i in range(28):
            nib = (data[b + 2 + i // 2] >> ((i & 1) * 4)) & 0xF
            nib = nib - 16 if nib >= 8 else nib
            s = ((nib << 12) >> shift) + ((s1 * f0 + s2 * f1 + 32) >> 6)
            s = -32768 if s < -32768 else 32767 if s > 32767 else s
            out.append(s)
            s2, s1 = s1, s
        if flags & 1 and not flags & 2:  # end without repeat: the loop-trap block after it is silence
            break
    return out


def write_wav(path, samples, rate):
    with wave.open(path, 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(samples.tobytes())


def read_bank(path):
    d = open(path, 'rb').read()
    assert d[:4] == b'PSFX', path
    n = struct.unpack_from('<H', d, 4)[0]
    base = 8 + 16 * n
    out = []
    for k in range(n):
        sid, _, off, size, rate = struct.unpack_from('<HHIII', d, 8 + 16 * k)
        out.append((sid, rate, d[base + off:base + off + size]))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('stage')
    ap.add_argument('--all', action='store_true')
    ap.add_argument('--only', nargs='+', default=[])
    ap.add_argument('--no-play', action='store_true')
    ap.add_argument('--src', default=os.path.join(REPO, '..', 'Sonic2_Extracted'))
    a = ap.parse_args()
    data, bc = os.path.join(a.src, 'Data'), os.path.join(a.src, 'Bytecode')
    iso = os.path.join(REPO, 'build', 'iso', 'Data')
    special = {'resident': 'PS1SfxResident.bin', 'menu': 'PS1SfxMenu.bin'}
    if a.stage.lower() in special:
        bank = os.path.join(iso, 'Game', special[a.stage.lower()])
        spaths = []
    else:
        bank = os.path.join(iso, 'Stages', a.stage, 'PS1Sfx.bin')
        spaths = sfx_sets.osh.stage_config(data, a.stage)[1]
    if not os.path.exists(bank):
        sys.exit('no bank %s (run tools/data_disc.sh first)' % os.path.relpath(bank))
    gnames, gpaths = sfx_sets.global_sfx(data)
    free = {}
    rates_txt = os.path.join(REPO, 'build', 'sfx_rates.txt')
    if os.path.exists(rates_txt):
        for line in open(rates_txt):
            w = line.split()
            if len(w) >= 5 and w[0] == a.stage:
                free[w[1]] = int(w[2])
    out_dir = os.path.join(REPO, 'build', 'listen', a.stage)
    os.makedirs(out_dir, exist_ok=True)
    seen, pairs = set(), []
    with tempfile.TemporaryDirectory() as tmp:
        for sid, rate, blob in read_bank(bank):
            src = gpaths[sid] if sid < len(gpaths) else spaths[sid - len(gpaths)]
            if src in seen:  # ids sharing a file share its data (Ring L / Ring R)
                continue
            seen.add(src)
            name = os.path.splitext(os.path.basename(src))[0]
            if a.only and not any(o.lower() in name.lower() for o in a.only):
                continue
            if not a.all and not a.only and rate >= 44100:
                continue
            k = len(pairs) + 1
            orig = os.path.join(out_dir, '%02d_%s_orig.wav' % (k, name))
            ps1 = os.path.join(out_dir, '%02d_%s_ps1_%d.wav' % (k, name, rate))
            pcm = os.path.join(tmp, 'o.wav')
            subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', os.path.join(data, 'SoundFX', src), '-ac', '1',
                            '-c:a', 'pcm_s16le', pcm], check=True)
            os.replace(pcm, orig)
            write_wav(ps1, spu_decode(blob), rate)
            with wave.open(orig) as w:
                src_rate = w.getframerate()
            pairs.append((name, src_rate, free.get(src), rate, orig, ps1))
    if not pairs:
        print('%s: nothing to play (every sound at 44,100 Hz? try --all)' % a.stage)
        return
    print('%s: %d sounds -> %s' % (a.stage, len(pairs), os.path.relpath(out_dir)))
    for name, src_rate, before, rate, orig, ps1 in pairs:
        cap = '' if before is None or before == rate else '  (%d Hz before the 400 KB cap)' % before
        print('  %-18s source %5d Hz -> PS1 %5d Hz%s' % (name, src_rate, rate, cap), flush=True)
        if not a.no_play:
            try:
                subprocess.run(['afplay', orig], check=False)
                subprocess.run(['sleep', '0.3'])
                subprocess.run(['afplay', ps1], check=False)
                subprocess.run(['sleep', '0.7'])
            except KeyboardInterrupt:
                print('stopped')
                return


if __name__ == '__main__':
    main()
