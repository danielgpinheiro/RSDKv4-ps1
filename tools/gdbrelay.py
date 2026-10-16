#!/usr/bin/env python3
"""GDB remote relay for PCSX-Redux's stub under GDB 18: the stub names its thread "p1.t1" (qC, qXfer:threads), not valid
multiprocess syntax (the thread id must be hex: "p1.1"); GDB 17 accepted it, 18 rejects it. Speaks the remote protocol
on stdin / stdout (target remote | gdbrelay.py PORT) and rewrites the stub's packets, checksums recomputed."""
import os, re, select, socket, sys
s = socket.create_connection(('127.0.0.1', int(sys.argv[1])))
buf = b''
PKT = re.compile(rb'\$([^#$]*)#([0-9a-fA-F]{2})')
def fix(body):
    body = body.replace(b'p1.t1', b'p1.1')
    return b'$' + body + b'#' + b'%02x' % (sum(body) & 0xFF)
while True:
    r, _, _ = select.select([0, s], [], [])
    if 0 in r:
        d = os.read(0, 65536)
        if not d: break
        s.sendall(d)
    if s in r:
        d = s.recv(65536)
        if not d: break
        buf += d
        out = b''
        while True:
            i = buf.find(b'$')
            if i < 0: out += buf; buf = b''; break
            out += buf[:i]; buf = buf[i:]
            m = PKT.match(buf)
            if not m:
                if b'#' in buf and len(buf) > buf.index(b'#') + 2: out += buf[:1]; buf = buf[1:]; continue  # not a packet start
                break  # incomplete packet: wait for more
            out += fix(m.group(1)) if b'p1.t1' in m.group(1) else m.group(0)
            buf = buf[m.end():]
        if out: os.write(1, out)
