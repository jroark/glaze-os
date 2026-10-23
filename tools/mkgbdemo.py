#!/usr/bin/env python3
"""Generate original CC0 Game Boy / Color cartridges; no Nintendo boot code/art.
Arrows move a diamond; A/B sound a tone, Start recentres. X persists in SRAM.
"""
import argparse
from pathlib import Path
import struct

class Code:
    def __init__(self): self.data=bytearray(); self.labels={}; self.fixups=[]
    def emit(self,*b): self.data.extend(b)
    def label(self,s): self.labels[s]=0x150+len(self.data)
    def jump(self,op,label):
        self.emit(op,0,0); self.fixups.append((len(self.data)-2,label))
    def ld(self,a): self.emit(0x3e,a)
    def write(self,a): self.emit(0xea,a&255,a>>8)
    def read(self,a): self.emit(0xfa,a&255,a>>8)
    def set(self,a,v): self.ld(v); self.write(a)
    def finish(self):
        for i,label in self.fixups: struct.pack_into('<H',self.data,i,self.labels[label])
        return self.data

def make(cgb):
    c=Code(); c.emit(0xf3,0x31,0xfe,0xff)
    c.set(0xff40,0); c.set(0,10)
    c.read(0xa000); c.emit(0xfe,8); c.jump(0xd2,'position')
    c.ld(80)
    c.label('position'); c.write(0xc001); c.set(0xc000,80)
    # Initialize tiles: blank, striped background, diamond sprite.
    tiles=bytes([0]*16+[0xaa,0x33,0x55,0x66]*4+[
        0x18,0x18,0x3c,0x24,0x7e,0x42,0xff,0x81,
        0xff,0x81,0x7e,0x42,0x3c,0x24,0x18,0x18])
    c.emit(0x21,0,0x80)
    for v in tiles: c.ld(v); c.emit(0x22)
    c.emit(0x21,0,0x98,0x01,0,4)
    c.label('map'); c.ld(1); c.emit(0x22,0x0b,0x78,0xb1); c.jump(0xc2,'map')
    c.set(0xff47,0xe4); c.set(0xff48,0xe4)
    if cgb:
        colors=struct.pack('<4H',0x7fff,0x7400,0x03e0,0x001f)
        for index,port in [(0xff68,0xff69),(0xff6a,0xff6b)]:
            c.set(index,0x80)
            for v in colors: c.set(port,v)
        c.set(0xff4d,1); c.emit(0x10,0) # Exercise CGB double speed.
    c.set(0xfe02,2); c.set(0xfe03,0)
    c.set(0xff26,0x80); c.set(0xff24,0x77); c.set(0xff25,0x11)
    c.set(0xff11,0x80); c.set(0xff12,0xf0); c.set(0xff13,0x80)
    c.set(0xff40,0x93)
    c.label('frame'); c.read(0xff44); c.emit(0xfe,144); c.jump(0xc2,'frame')
    c.set(0xff00,0x20); c.read(0xff00); c.emit(0x2f,0xe6,15); c.write(0xa001); c.emit(0x47)
    for bit,addr,op,name in [(0,0xc001,0x3c,'right'),(1,0xc001,0x3d,'left'),(2,0xc000,0x3d,'up'),(3,0xc000,0x3c,'down')]:
        c.emit(0xcb,0x40+bit*8); c.jump(0xca,name); c.read(addr); c.emit(op); c.write(addr); c.label(name)
    c.set(0xff00,0x10); c.read(0xff00); c.emit(0x2f,0xe6,15); c.write(0xa002); c.emit(0x47)
    c.emit(0xe6,3); c.jump(0xca,'quiet'); c.set(0xff14,0x87)
    c.jump(0xc3,'buttons'); c.label('quiet'); c.set(0xff12,0)
    c.label('buttons'); c.emit(0xcb,0x58); c.jump(0xca,'draw'); c.set(0xc001,80); c.set(0xc000,80)
    c.label('draw'); c.set(0xff12,0xf0)
    c.read(0xc001); c.write(0xfe01); c.write(0xa000)
    c.read(0xc000); c.write(0xfe00)
    c.read(0xa003); c.emit(0x3c); c.write(0xa003)
    c.label('wait'); c.read(0xff44); c.emit(0xfe,144); c.jump(0xca,'wait'); c.jump(0xc3,'frame')
    rom=bytearray(32768); rom[0x100:0x104]=bytes([0,0xc3,0x50,1])
    rom[0x134:0x13e]=b'GLAZE DEMO'; rom[0x143]=0xc0 if cgb else 0
    rom[0x147]=0x10; rom[0x148]=0; rom[0x149]=2
    code=c.finish(); rom[0x150:0x150+len(code)]=code
    checksum=0
    for x in rom[0x134:0x14d]: checksum=(checksum-x-1)&255
    rom[0x14d]=checksum
    struct.pack_into('>H',rom,0x14e,sum(rom)&65535)
    return rom

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir',type=Path,default=Path(__file__).resolve().parents[1]/'out/pkg-res/gameboy')
    parser.add_argument('--mode',choices=['dmg','cgb','both'],default='both')
    parser.add_argument('--icon',type=Path,help='optionally regenerate the original icon (requires Pillow)')
    args=parser.parse_args()
    args.output_dir.mkdir(parents=True,exist_ok=True)
    if args.mode in ('dmg','both'): (args.output_dir/'demo.gb').write_bytes(make(False))
    if args.mode in ('cgb','both'): (args.output_dir/'demo.gbc').write_bytes(make(True))
    if not args.icon: return
    # Original icon: a compact handheld on a blue rounded square.
    from PIL import Image,ImageDraw
    im=Image.new('RGB',(57,57),'#2e6caa'); d=ImageDraw.Draw(im)
    d.rounded_rectangle((13,5,44,52),radius=5,fill='#d9dde2')
    d.rounded_rectangle((17,10,40,31),radius=2,fill='#263346')
    d.rectangle((20,13,37,28),fill='#9bb58b')
    d.rectangle((21,35,25,46),fill='#273246'); d.rectangle((17,39,29,42),fill='#273246')
    d.ellipse((34,35,39,40),fill='#b82764'); d.ellipse((29,41,34,46),fill='#b82764')
    raw=bytearray(b'GZI1'+struct.pack('<HHHH',57,57,0,0))
    rgb=im.tobytes()
    for i in range(0,len(rgb),3):
        r,g,b=rgb[i:i+3]
        raw+=struct.pack('<H',(r>>3)<<11|(g>>2)<<5|(b>>3))
    args.icon.parent.mkdir(parents=True,exist_ok=True)
    args.icon.write_bytes(raw)
if __name__=='__main__': main()
