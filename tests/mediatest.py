#!/usr/bin/env python3
"""Compares out/host/mediatest's decoding with ffmpeg's.

    tests/mediatest.py [files...]     (default: userdata/Music/*, userdata/Movies/*
                                       and MPEG-1 streams made with ffmpeg)

Needs ffmpeg and numpy. Prints the SNR of each decode against ffmpeg's, and
of a resampled decode against ffmpeg's resampler (linear interpolation is
expected to be well below the straight decode). For MPEG-1 (.mpg) it also
compares every decoded frame with ffmpeg's, as RGB565.
"""
import glob, os, re, subprocess, sys, tempfile
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOL = os.path.join(ROOT, 'out', 'host', 'mediatest')


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


def has_audio(path):
    return bool(subprocess.run(['ffprobe', '-v', 'error', '-select_streams', 'a', '-show_entries',
                                'stream=index', '-of', 'csv=p=0', path],
                               check=True, capture_output=True, text=True).stdout.strip())


def rgb565(a):
    a = a.astype(np.uint32)
    return np.stack([(a >> 11) << 3, ((a >> 5) & 63) << 2, (a & 31) << 3], -1).astype(np.float64)


def check_frames(path, got_path, period):
    """PSNR of our RGB565 frames against ffmpeg's decode of the same file,
    and of the frames shown after seeks (`period` ms apart)."""
    info = subprocess.run(['ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_entries',
                           'stream=width,height', '-of', 'csv=p=0', path],
                          check=True, capture_output=True, text=True).stdout.split(',')
    w, h = int(info[0]), int(info[1])
    ref = subprocess.run(['ffmpeg', '-v', 'error', '-i', path, '-an', '-f', 'rawvideo',
                          '-pix_fmt', 'rgb565le', '-'], check=True, capture_output=True).stdout
    ref = np.frombuffer(ref, '<u2').reshape(-1, h, w)
    got = np.fromfile(got_path, '<u2').reshape(-1, h, w)
    n = min(len(ref), len(got))

    def psnr(a, b):
        return 10 * np.log10(255 ** 2 / (np.mean((rgb565(a) - rgb565(b)) ** 2) + 1e-9))

    # Each seek's frame against the decode's frame at the time it says.
    seeks = np.fromfile(got_path + '.seek', np.uint8)
    rec = 4 + 2 * w * h
    worst = float('inf')
    for k in range(len(seeks) // rec):
        r = seeks[k * rec:(k + 1) * rec]
        t = int(r[:4].view('<u4')[0])
        i = min(int(round(t / period)), n - 1)
        worst = min(worst, psnr(ref[i], r[4:].view('<u2').reshape(h, w)))
    return psnr(ref[:n], got[:n]), len(got), len(ref), worst


# MPEG-1 program streams in the shapes Videos meets: open GOPs with B frames,
# none, no sound; mono and stereo, MPEG-1 and MPEG-2 LSF Layer II; odd sizes.
MPEG_STREAMS = {
    'qvga_b2_22k.mpg': ('320x240', '25', 10, '-bf 2 -g 15 -b:v 300k', '-ar 22050 -ac 1 -b:a 64k'),
    'vcd_stereo.mpg': ('352x240', '30000/1001', 6, '-bf 2 -g 18 -b:v 600k', '-ar 44100 -ac 2 -b:a 128k'),
    'nob_32k.mpg': ('240x176', '24', 5, '-bf 0 -g 12 -b:v 200k', '-ar 32000 -ac 1 -b:a 48k'),
    'odd_48k.mpg': ('238x178', '25', 4, '-bf 2 -b:v 250k', '-ar 48000 -ac 1 -b:a 96k'),
    'silent.mpg': ('160x120', '25', 4, '-bf 2 -b:v 150k', None),
}


def make_mpeg(tmp):
    out = []
    for name, (size, rate, secs, vopt, aopt) in MPEG_STREAMS.items():
        path = os.path.join(tmp, name)
        cmd = ['ffmpeg', '-v', 'error', '-y', '-f', 'lavfi', '-i', f'testsrc2=size={size}:rate={rate}']
        if aopt:
            cmd += ['-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000']
        cmd += ['-t', str(secs), '-c:v', 'mpeg1video', *vopt.split()]
        cmd += ['-c:a', 'mp2', *aopt.split()] if aopt else ['-an']
        subprocess.run(cmd + ['-f', 'mpeg', path], check=True)
        out.append(path)
    return out


def main():
    files = sys.argv[1:] or sorted(glob.glob(os.path.join(ROOT, 'userdata/Music/*')) +
                                   glob.glob(os.path.join(ROOT, 'userdata/Movies/*')))
    bad = 0
    with tempfile.TemporaryDirectory() as tmp:
        if not sys.argv[1:]:
            files += make_mpeg(tmp)
        raw = os.path.join(tmp, 'out.raw')
        frames = os.path.join(tmp, 'frames.rgb565')
        for f in files:
            mpg = f.lower().endswith('.mpg')
            r = subprocess.run([TOOL, f, raw] + (['0', frames] if mpg else []), capture_output=True, text=True)
            print(r.stdout.strip())
            if r.returncode:
                print('FAIL', f, r.stderr.strip())
                bad += 1
                continue
            if mpg:
                period = int(re.search(r'(\d+) us/frame', r.stdout).group(1)) / 1000
                p, n, nref, sp = check_frames(f, frames, period)
                vok = p > 30 and n == nref and sp > 30
                bad += not vok
                print(f'{"PASS" if vok else "FAIL"} {os.path.basename(f)} video: {p:.1f} dB PSNR, '
                      f'{n} frames (ffmpeg {nref}), seeks {sp:.1f} dB at worst')
                if not has_audio(f):
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
