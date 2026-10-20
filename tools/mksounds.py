#!/usr/bin/env python3
"""Synthesizes the system sounds (servers/audio/sound.c) as 22050 Hz mono
16-bit WAVs: UI clicks and the alert tones of glaze/audio.h. With --aim, it
makes the AIM package's buddy and message sounds instead (packages/aim/res),
in the spirit of the classic ones rather than copies of them.

    tools/mksounds.py assets/sounds
    tools/mksounds.py --aim packages/aim/res

The output is deterministic and checked in; rerun it after changing a sound.
"""

import math
import os
import random
import struct
import sys
import wave

RATE = 22050


def secs(n):
    return int(n * RATE)


def silence(dur):
    return [0.0] * secs(dur)


def tone(freq, dur, partials=((1, 1.0),), decay=8.0, attack=0.002):
    """A struck tone: sine partials (ratio, level) with exponential decay."""
    out = []
    for i in range(secs(dur)):
        t = i / RATE
        env = math.exp(-decay * t) * min(1.0, t / attack if attack else 1.0)
        out.append(env * sum(a * math.sin(2 * math.pi * freq * r * t) for r, a in partials))
    return out


def noise(dur, decay, lowpass, rng, attack=0.0005):
    """Decaying noise through a one-pole low-pass (`lowpass` in 0..1)."""
    out, y = [], 0.0
    for i in range(secs(dur)):
        t = i / RATE
        y += lowpass * (rng.uniform(-1, 1) - y)
        out.append(y * math.exp(-decay * t) * min(1.0, t / attack))
    return out


def mix(*parts):
    n = max(len(p) for p in parts)
    return [sum(p[i] for p in parts if i < len(p)) for i in range(n)]


def concat(*parts):
    return [s for p in parts for s in p]


def place(base, part, at):
    """Adds `part` into `base` starting `at` seconds in."""
    start = secs(at)
    need = start + len(part)
    if len(base) < need:
        base = base + [0.0] * (need - len(base))
    for i, s in enumerate(part):
        base[start + i] += s
    return base


def normalize(samples, peak):
    top = max(abs(s) for s in samples) or 1.0
    return [s * peak / top for s in samples]


def fade_out(samples, dur=0.005):
    n = min(len(samples), secs(dur))
    for i in range(n):
        samples[len(samples) - n + i] *= 1 - (i + 1) / n
    return samples


def write(path, samples):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b"".join(struct.pack("<h", max(-32767, min(32767, round(s * 32767)))) for s in samples))


def sounds():
    rng = random.Random(300)
    s = {}

    # Clicks: short and quiet, since they come with every touch.
    s["tap.wav"] = normalize(mix(noise(0.012, 400, 0.5, rng), tone(2400, 0.012, decay=350)), 0.30)
    s["key.wav"] = normalize(mix(noise(0.030, 220, 0.7, rng), tone(1800, 0.020, decay=300)), 0.45)
    whoosh, y = [], 0.0
    for i in range(secs(0.16)):
        t = i / 0.16 / RATE
        y += (0.05 + 0.25 * t) * (rng.uniform(-1, 1) - y)
        whoosh.append(y * math.sin(math.pi * t) ** 2)
    s["swipe.wav"] = normalize(whoosh, 0.35)
    s["lock.wav"] = normalize(mix(tone(170, 0.07, decay=70), noise(0.03, 160, 0.6, rng)), 0.6)
    s["unlock.wav"] = normalize(concat(noise(0.02, 200, 0.6, rng), mix(tone(240, 0.06, decay=80),
                                                                        noise(0.03, 150, 0.6, rng))), 0.55)

    # Alert tones.
    bellish = ((1, 1.0), (2, 0.3), (3, 0.12))
    tri = []
    for k, f in enumerate((1318.5, 1046.5, 1568.0)):
        tri = place(tri, tone(f, 0.45, bellish, decay=9), k * 0.14)
    s["tritone.wav"] = normalize(tri, 0.7)

    s["chime.wav"] = normalize(mix(tone(880, 1.6, ((1, 1.0), (2.76, 0.4), (5.4, 0.15)), decay=3),
                                   place([], tone(1318.5, 1.3, ((1, 0.8), (2.76, 0.3)), decay=3.5), 0.25)), 0.7)

    s["bell.wav"] = normalize(tone(523.25, 2.0, ((0.5, 0.4), (1, 1.0), (2.0, 0.5), (2.4, 0.3), (3.0, 0.2),
                                                 (4.2, 0.1)), decay=2.2), 0.7)

    mar = []
    for k, f in enumerate((523.25, 659.25, 783.99, 1046.5, 783.99)):
        mar = place(mar, tone(f, 0.35, ((1, 1.0), (4, 0.25), (9.2, 0.05)), decay=14), k * 0.12)
    s["marimba.wav"] = normalize(mar, 0.7)

    beep = tone(1000, 0.09, ((1, 1.0), (3, 0.25), (5, 0.1)), decay=0, attack=0.004)
    beep = fade_out(beep, 0.01)
    s["alarm.wav"] = normalize(concat(*(concat(beep, silence(0.07)) for _ in range(4))), 0.65)

    return {k: fade_out(v) for k, v in s.items()}


def sweep(f0, f1, dur, partials=((1, 1.0),), level=1.0):
    """A tone gliding from f0 to f1 with a sine-shaped swell."""
    out, phase = [], 0.0
    n = secs(dur)
    for i in range(n):
        t = i / n
        phase += 2 * math.pi * (f0 + (f1 - f0) * t) / RATE
        out.append(level * math.sin(math.pi * t) * sum(a * math.sin(r * phase) for r, a in partials))
    return out


def aim_sounds():
    rng = random.Random(1997)
    s = {}

    # A buddy signs on: a hinge's creak rising, then the latch.
    creak = sweep(330, 520, 0.42, ((1, 1.0), (2, 0.5), (3, 0.35), (5, 0.2)))
    for i in range(len(creak)):
        creak[i] *= 0.6 + 0.4 * math.sin(2 * math.pi * 38 * i / RATE)
    s["dooropen.wav"] = normalize(place(creak, mix(noise(0.05, 120, 0.5, rng), tone(1400, 0.04, decay=120)),
                                        0.40), 0.6)

    # A buddy signs off: a low thump with a rattle.
    s["doorslam.wav"] = normalize(mix(tone(70, 0.35, ((1, 1.0), (2.1, 0.4)), decay=14),
                                      noise(0.25, 22, 0.35, rng), place([], noise(0.12, 40, 0.8, rng), 0.05)), 0.7)

    # Messages: a rising two-note blip in, a falling pop out.
    blip = ((1, 1.0), (2, 0.2))
    s["imrcv.wav"] = normalize(place(tone(784, 0.16, blip, decay=24), tone(1175, 0.22, blip, decay=20), 0.09), 0.6)
    s["imsend.wav"] = normalize(concat(sweep(1500, 700, 0.07, ((1, 1.0),)), tone(620, 0.08, blip, decay=60)), 0.5)

    return {k: fade_out(v) for k, v in s.items()}


def main():
    args = sys.argv[1:]
    make = sounds
    if args[:1] == ["--aim"]:
        make = aim_sounds
        args = args[1:]
    if len(args) != 1:
        sys.exit("usage: mksounds.py [--aim] <out dir>")
    os.makedirs(args[0], exist_ok=True)
    for name, samples in sorted(make().items()):
        write(os.path.join(args[0], name), samples)


if __name__ == "__main__":
    main()
