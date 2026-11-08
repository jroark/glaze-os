"""Screenshot comparison against emulator references (numpy only, no PIL).

The emulator runs the same input script through tests/run.sh; its screenshot is
the reference. A differing pair is a device bug or an emulator model gap, so
both images and a diff are kept for triage.
"""
import hashlib
import os
import struct
import subprocess
import zlib

import numpy as np

from .device import ROOT
from . import images


def read_png(path):
    data = open(path, 'rb').read()
    assert data[:8] == b'\x89PNG\r\n\x1a\n', '%s is not a PNG' % path
    pos, idat, w = 8, b'', None
    while pos < len(data):
        n, typ = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if typ == b'IHDR':
            w, h, depth, ctype, _, _, interlace = struct.unpack('>IIBBBBB', body)
            assert depth == 8 and ctype in (2, 6) and not interlace, 'unsupported PNG %s' % path
            bpp = 3 if ctype == 2 else 4
        elif typ == b'IDAT':
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat)
    stride = w * bpp
    out = np.zeros((h, stride), dtype=np.uint8)
    prev = np.zeros(stride, dtype=np.int32)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = np.frombuffer(raw, dtype=np.uint8, count=stride, offset=y * (stride + 1) + 1).astype(np.int32)
        if f == 0:
            cur = line
        elif f == 2:
            cur = (line + prev) & 0xFF
        else:
            cur = np.zeros(stride, dtype=np.int32)
            for x in range(stride):
                a = cur[x - bpp] if x >= bpp else 0
                b = prev[x]
                c = prev[x - bpp] if x >= bpp else 0
                if f == 1:
                    p = a
                elif f == 3:
                    p = (a + b) >> 1
                else:
                    pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                    p = a if pa <= pb and pa <= pc else b if pb <= pc else c
                cur[x] = (line[x] + p) & 0xFF
        out[y] = cur
        prev = cur
    return out.reshape(h, w, bpp)[:, :, :3]


def write_png(path, rgb):
    h, w, _ = rgb.shape
    raw = b''.join(b'\0' + rgb[y].astype(np.uint8).tobytes() for y in range(h))
    def chunk(t, b):
        return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xFFFFFFFF)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def compare(hw_png, ref_png, diff_png, masks=((0, 0, 240, 16),), tolerance=40):
    """Returns the fraction of unmasked pixels whose largest channel difference
    exceeds `tolerance`. masks are (x0, y0, x1, y1); the default hides the
    status bar's clock, battery and transport badges."""
    a = read_png(hw_png).astype(np.int32)
    b = read_png(ref_png).astype(np.int32)
    assert a.shape == b.shape, 'size %s vs %s' % (a.shape, b.shape)
    # The emulator's screenshot carries its backlight/dim model, the device's
    # is the raw framebuffer: scale the reference by the median ratio of lit
    # pixels so only rendering and state differences count.
    lit = (a.sum(axis=2) > 60) & (b.sum(axis=2) > 30)
    gain = float(np.median(a[lit] / np.maximum(b[lit], 1))) if lit.any() else 1.0
    compare.gain = gain
    if 0.9 < gain < 1.1:
        gain = 1.0
    b = np.clip(b * gain, 0, 255)
    d = np.abs(a - b).max(axis=2)
    keep = np.ones(d.shape, dtype=bool)
    for x0, y0, x1, y1 in masks:
        keep[y0:y1, x0:x1] = False
    bad = (d > tolerance) & keep
    vis = (a * 0.35).astype(np.int32)
    vis[bad] = [255, 0, 255]
    write_png(diff_png, np.clip(vis, 0, 255))
    return bad.sum() / keep.sum()


def emulator_ref(script, seconds, env=None, log=print):
    """Runs `script` in the emulator through tests/run.sh and returns the
    screenshot path, cached by script text, flags and source state."""
    text = open(os.path.join(ROOT, script)).read()
    key = hashlib.sha1(('%s|%s|%s|%s|%s' % (text, seconds, sorted((env or {}).items()), images.describe(),
                                            images.tree_hash())).encode()).hexdigest()[:12]
    name = os.path.splitext(os.path.basename(script))[0]
    ref = os.path.join(ROOT, 'out', 'hw', 'emu-refs', '%s-%s.png' % (name, key))
    if os.path.exists(ref):
        return ref
    os.makedirs(os.path.dirname(ref), exist_ok=True)
    log('emulator reference: %s (%ss)' % (script, seconds))
    e = dict(os.environ, **(env or {}))
    e.setdefault('SDL_VIDEODRIVER', 'dummy')
    with open(ref + '.log', 'w') as f:
        r = subprocess.run(['tests/run.sh', script, str(seconds)], cwd=ROOT, env=e, stdout=f,
                           stderr=subprocess.STDOUT)
    shot = os.path.join(ROOT, 'out', 'shots', name + '.png')
    if r.returncode or not os.path.exists(shot):
        raise RuntimeError('emulator run of %s failed; see %s.log' % (script, ref))
    os.replace(shot, ref)
    return ref
