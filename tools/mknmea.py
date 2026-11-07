#!/usr/bin/env python3
"""Writes a synthetic NMEA-0183 track for be300's --cf-gps: a SiRF-style
second (GGA, GSA, GSV cycle, RMC) per point, going round a 60 m circle (one lap per run, three
seconds without a fix first, as at a cold start).

    tools/mknmea.py tests/gps/track.nmea [lat lon seconds]
"""
import math, sys, datetime

def ck(body):
    x = 0
    for c in body:
        x ^= ord(c)
    return f"${body}*{x:02X}\r\n"

def dm(v, lat):
    a = abs(v); d = int(a); m = (a - d) * 60
    return (f"{d:02d}{m:07.4f}" if lat else f"{d:03d}{m:07.4f}"), ("N" if v >= 0 else "S") if lat else ("E" if v >= 0 else "W")

out = sys.argv[1]
lat0 = float(sys.argv[2]) if len(sys.argv) > 2 else 37.795500
lon0 = float(sys.argv[3]) if len(sys.argv) > 3 else -122.393700
secs = int(sys.argv[4]) if len(sys.argv) > 4 else 120
sats = [(7, 79, 48, 42), (2, 51, 62, 43), (26, 36, 256, 42), (27, 27, 138, 40),
        (9, 23, 313, 39), (4, 19, 159, 37), (15, 12, 41, 0), (10, 8, 290, 0)]
t0 = datetime.datetime(2026, 11, 6, 18, 0, 0)
r = 60.0   # metres: a circle walked in secs seconds
lines = []
prev = None
for i in range(secs):
    a = 2 * math.pi * i / secs
    lat = lat0 + (r * math.sin(a)) / 111195
    lon = lon0 + (r * (1 - math.cos(a))) / (111195 * math.cos(math.radians(lat0)))
    t = t0 + datetime.timedelta(seconds=i)
    hms = t.strftime("%H%M%S") + ".00"
    la, ns = dm(lat, True); lo, ew = dm(lon, False)
    speed_kn = 2 * math.pi * r / secs / 0.514444
    course = math.degrees(a) % 360            # the circle's tangent: north, then east
    fixq = 0 if i < 3 else 1                  # a cold start: three seconds without a fix
    used = [s for s in sats if s[3] > 0]
    lines.append(ck(f"GPGGA,{hms},{la},{ns},{lo},{ew},{fixq},{len(used) if fixq else 0:02d},1.2,12.3,M,-28.0,M,,0000"))
    lines.append(ck("GPGSA,A,{},{},,{}".format(3 if fixq else 1,
                    ",".join(f"{s[0]:02d}" for s in used) + "," * (12 - len(used) - 1), "2.0,1.2,1.6")))
    groups = [sats[k:k + 4] for k in range(0, len(sats), 4)]
    for n, g in enumerate(groups, 1):
        body = f"GPGSV,{len(groups)},{n},{len(sats):02d}"
        for s in g:
            body += f",{s[0]:02d},{s[1]:02d},{s[2]:03d}," + (f"{s[3]:02d}" if s[3] else "")
        lines.append(ck(body))
    lines.append(ck(f"GPRMC,{hms},{'A' if fixq else 'V'},{la},{ns},{lo},{ew},{speed_kn:.2f},{course:.2f},{t.strftime('%d%m%y')},,,A"))
open(out, "w", newline="").write("".join(lines))
