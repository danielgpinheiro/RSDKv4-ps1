#!/usr/bin/env python3
"""Load-time report from a tools/load_profile.sh log (docs/30 phase 9.1; from the Sonic CD port's tool).

Per load (from its GOTO, or the previous load's end, to its LOAD line): vblanks, requests (blocking reads + load-stream
sessions: each one seek), stream starts, sectors, heap never reached at the load's end; then
  cd    the files the requests read (LBA -> file through the disc image's ISO9660 tree), by time inside the requests;
  open  the vblanks from each fopen to the next one (or the load's end): where the load spends its time, CPU included.
Usage: load_profile.py LOG [DISC.bin]
"""
import collections, re, struct, sys


def disc_files(path):
    f = open(path, 'rb')

    def sec(lba):
        f.seek(lba * 2352 + 24)
        return f.read(2048)

    files = []

    def walk(lba, size, prefix):
        for s in range((size + 2047) // 2048):
            d, o = sec(lba + s), 0
            while o < 2048 and d[o]:
                ln = d[o]
                el, sz = struct.unpack_from('<I', d, o + 2)[0], struct.unpack_from('<I', d, o + 10)[0]
                nm = d[o + 33:o + 33 + d[o + 32]]
                if nm not in (b'\0', b'\1'):
                    name = prefix + nm.decode(errors='replace').split(';')[0]
                    if d[o + 25] & 2:
                        walk(el, sz, name + '/')
                    else:
                        files.append((el, max(1, (sz + 2047) // 2048), name))
                o += ln

    pvd = sec(16)
    walk(struct.unpack_from('<I', pvd, 158)[0], struct.unpack_from('<I', pvd, 166)[0], '/')
    return sorted(files)


def kv(line):
    return dict(re.findall(r'(\w+)=(\S*)', line))


def main():
    log = [l.rstrip('\n') for l in open(sys.argv[1])]
    files = disc_files(sys.argv[2] if len(sys.argv) > 2 else 'build/game.bin')

    def which(lba):
        for el, n, name in files:
            if el <= lba < el + n:
                return name.lstrip('/')
        return '(directory / system)'

    start = {'vbl': 0, 'requests': 0, 'streams': 0, 'sectors': 0}
    reqs, opens, rows = [], [], []
    last_counts = dict(start)
    for l in log:
        if l.startswith('GOTO '):
            d = kv(l)
            start = {'vbl': int(d['vbl']), 'requests': int(d['requests']), 'streams': int(d['streams']),
                     'sectors': last_counts['sectors']}
            reqs, opens = [], []
        elif l.startswith('REQ '):
            d = kv(l)
            reqs.append((int(d['lba']), int(d['n']), int(d['us'])))
        elif l.startswith('OPEN '):
            m = re.match(r'OPEN vbl=(\d+) (.*)', l)
            opens.append((int(m[1]), m[2]))
        elif l.startswith('LOAD '):
            d = kv(l)
            vbl, rq, st, se = int(d['vbl']), int(d['requests']), int(d['streams']), int(d['sectors'])
            name = d['folder'] or 'boot'
            if d['list'] != '0' or d['pos'] != '0' or d['folder']:
                name = '%s (list %s pos %s)' % (d['folder'] or '-', d['list'], d['pos'])
            rows.append((name, vbl - start['vbl'], rq - start['requests'], st - start['streams'], se - start['sectors'],
                         int(d['headroom']), list(reqs), list(opens), vbl))
            start = {'vbl': vbl, 'requests': rq, 'streams': st, 'sectors': se}
            last_counts = dict(start)
            reqs, opens = [], []
        elif l.startswith('DONE') or l.startswith('GUARDS'):
            print(l)
    print('%-28s %8s %6s %5s %7s %9s' % ('load', 'vblanks', 'req', 'seeks', 'sectors', 'headroom'))
    for name, v, rq, st, se, hr, _, _, _ in rows:
        print('%-28s %8d %6d %5d %7d %9d' % (name[:28], v, rq, st, se, hr))
    for name, v, rq, st, se, hr, rs, ops, end in rows:
        print('\n== %s: %d vblanks (%.2f s)' % (name, v, v / 60))
        per = collections.defaultdict(lambda: [0, 0, 0])
        for lba, cnt, u in rs:
            p = per[which(lba)]
            p[0] += 1; p[1] += cnt; p[2] += u
        for fname, (c, sc, u) in sorted(per.items(), key=lambda kv: -kv[1][2])[:8]:
            print('  cd   %-50s %3d req %4d sect %6.2f s' % (fname[-50:], c, sc, u / 1e6))
        gaps = collections.defaultdict(int)
        for i, (vb, path) in enumerate(ops):
            nxt = ops[i + 1][0] if i + 1 < len(ops) else end
            gaps[path] += nxt - vb
        for path, g in sorted(gaps.items(), key=lambda kv: -kv[1])[:10]:
            if g:
                print('  open %-50s %4d vblanks' % (path[-50:], g))


if __name__ == '__main__':
    main()
