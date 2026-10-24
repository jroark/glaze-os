#!/usr/bin/env python3
"""Regenerate Glaze's original Sega and TurboGrafx app icons (Pillow required)."""
from pathlib import Path
from PIL import Image, ImageDraw
import struct
root=Path(__file__).resolve().parents[1]
for app,bg in [('mastersystem','#c43b34'),('genesis','#4257a0'),('gamegear','#348c99'),('turbografx16','#ba6c28')]:
    im=Image.new('RGB',(57,57),bg);d=ImageDraw.Draw(im)
    d.rounded_rectangle((4,8,52,33),radius=5,fill='#242831',outline='#cbd0dc',width=2)
    if app=='gamegear':
        d.rectangle((18,12,39,29),fill='#78b4ba',outline='#11151b',width=2)
        d.rectangle((9,18,14,21),fill='#c1c6d2');d.rectangle((10,16,12,23),fill='#c1c6d2')
        d.ellipse((44,19,47,22),fill='#d44249')
    elif app=='turbografx16':
        d.rectangle((10,12,35,29),fill='#10151c');d.line((14,16,31,16),fill='#d17a38',width=3)
        d.rectangle((40,13,46,25),fill='#9ba4ad')
    elif app=='mastersystem':
        d.rectangle((9,12,46,16),fill='#ab2f2b');d.rectangle((12,20,42,24),fill='#11151b')
    else:
        d.ellipse((15,11,42,29),fill='#080d15',outline='#707a89',width=2)
        d.ellipse((27,18,31,22),fill='#3764c1')
    d.line((29,33,29,39),fill='#b7becc',width=2)
    d.rounded_rectangle((9,38,47,51),radius=5,fill='#1c2027',outline='#b7becc')
    d.rectangle((16,40,19,48),fill='#c1c6d2');d.rectangle((13,43,22,46),fill='#c1c6d2')
    for x,y in [(33,43),(39,41)]+([(27,45)] if app=='genesis' else []):d.ellipse((x,y,x+3,y+3),fill='#c52b3c')
    data=bytearray(b'GZI1'+struct.pack('<HHHH',57,57,0,0));rgb=im.tobytes()
    for i in range(0,len(rgb),3):
        r,g,b=rgb[i:i+3];data+=struct.pack('<H',(r>>3)<<11|(g>>2)<<5|(b>>3))
    (root/f'packages/{app}/icon.gzi').write_bytes(data)
