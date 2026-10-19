#!/usr/bin/env python3
"""Compares host/mediatest's decoding with ffmpeg's.

    tests/mediatest.py [files...]     (default: userdata/Music/*, userdata/Movies/*)

Needs ffmpeg and numpy. Prints the SNR of each decode against ffmpeg's, and
of a resampled decode against ffmpeg's resampler (linear interpolation is
expected to be well below the straight decode).
"""
import glob, os, subprocess, sys, tempfile
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOL = os.path.join(ROOT, 'host', 'mediatest')


def ffmpeg_pcm(path, extra=()):
    out = subprocess.run(['ffmpeg', '-v', 'error', '-i', path, '-vn', *extra, '-f', 's16le', '-'],
                         check=True, capture_output=True).stdout
    return np.frombuffer(out, '<i2').astype(np.float64)


def snr(ref, got):
    """SNR in dB after aligning on the cross-correlation peak (ffmpeg trims
    the encoder delay a LAME header declares; Helix doesn't)."""
    n = min(len(ref), len(got), 1 << 18)
    if n < 1024:
        return float('-inf'), 0
    size = 1 << (2 * n - 1).bit_length()
    xc = np.fft.irfft(np.fft.rfft(ref[:n], size) * np.conj(np.fft.rfft(got[:n], size)), size)
    w = 8192
    lag = int(np.argmax(np.concatenate([xc[-w:], xc[:w + 1]]))) - w
    a = ref[max(0, lag):]
    b = got[max(0, -lag):]
    m = min(len(a), len(b))
    err = np.sum((a[:m] - b[:m]) ** 2) + 1e-9
    return 10 * np.log10(np.sum(a[:m] ** 2) / err), lag


def main():
    files = sys.argv[1:] or sorted(glob.glob(os.path.join(ROOT, 'userdata/Music/*')) +
                                   glob.glob(os.path.join(ROOT, 'userdata/Movies/*')))
    bad = 0
    with tempfile.TemporaryDirectory() as tmp:
        raw = os.path.join(tmp, 'out.raw')
        for f in files:
            r = subprocess.run([TOOL, f, raw], capture_output=True, text=True)
            print(r.stdout.strip())
            if r.returncode:
                print('FAIL', f, r.stderr.strip())
                bad += 1
                continue
            got = np.fromfile(raw, '<i2').astype(np.float64)
            s, lag = snr(ffmpeg_pcm(f), got)
            ok = s > 40
            bad += not ok
            print(f'{"PASS" if ok else "FAIL"} {os.path.basename(f)}: {s:.1f} dB (lag {lag})')
            subprocess.run([TOOL, f, raw, '44100'], check=True, capture_output=True)
            got = np.fromfile(raw, '<i2').astype(np.float64)
            s, lag = snr(ffmpeg_pcm(f, ['-ar', '44100']), got)
            print(f'     resampled to 44100: {s:.1f} dB (lag {lag})')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
