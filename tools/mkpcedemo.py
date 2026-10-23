#!/usr/bin/env python3
"""Original CC0 HuC6280 cartridge: striped tiles, diamond, pad reads and PSG.
ROM binaries are generated only under ignored out/ by default.
"""
from pathlib import Path
import argparse
import struct

class HuC:
    def __init__(self):self.code=bytearray();self.labels={};self.fix=[]
    def e(self,*v):self.code.extend(v)
    def label(self,n):self.labels[n]=0xe000+len(self.code)
    def lda(self,v):self.e(0xa9,v)
    def addr(self,op,a):self.e(op,a&255,a>>8)
    def store(self,a,v):self.lda(v);self.addr(0x8d,a)
    def jump(self,n):self.e(0x4c,0,0);self.fix.append((len(self.code)-2,n))
    def branch_zero(self,n):self.e(0xd0,3);self.jump(n)
    def reg(self,i,v):self.e(3,i,0x13,v&255,0x23,v>>8)
    def vram(self,a):self.reg(0,a);self.e(3,2)
    def word(self,v):self.e(0x13,v&255,0x23,v>>8)
    def done(self):
        for at,n in self.fix:struct.pack_into('<H',self.code,at,self.labels[n])
        return self.code

def demo():
    h=HuC();h.e(0x78,0xd8,0xd4,0xa2,255,0x9a)
    for page,bank in [(1,0xf8),(0,0xff),(2,0xf7)]:h.lda(bank);h.e(0x53,1<<page)
    h.addr(0xad,0x4010);h.e(0xc9,8,0xb0,2);h.lda(120);h.addr(0x8d,0x2000)
    h.store(0x2001,80)
    for i,v in [(5,0),(6,0),(7,0),(8,0),(9,0),(10,0x0202),(11,0x041f),(12,0x0b02),(13,239),(14,7),(15,0x10),(19,0x6000)]:h.reg(i,v)
    # Background tile 256, four bitplanes in two groups of eight words.
    h.vram(0x1000)
    for v in [0xccf0]*8+[0x00aa]*8:h.word(v)
    h.vram(0);h.e(0xa2,0,0xa0,4)
    h.label('map');h.word(0x100);h.e(0xca,0xd0,0xf9,0x88,0xd0,0xf6)
    # 16x16 diamond sprite, one bitplane repeated four times.
    h.vram(0x2000)
    masks=[((1<<(2*(min(y,15-y)+1)))-1)<<(7-min(y,15-y)) for y in range(16)]
    for _ in range(4):
        for mask in masks:h.word(mask)
    # Clear SAT: the first sprite is rewritten each frame.
    h.vram(0x6000);h.e(0xa2,0)
    h.label('satclear');h.word(0);h.e(0xca,0xd0,0xf9)
    colors=[0,0x038,0x1c0,7,0x1ff,0x1f8,0x03f,0x1c7]+[0]*7+[0x1ff]
    for start in (0,256):
        h.store(0x402,start&255);h.store(0x403,start>>8)
        for c in colors:h.store(0x404,c&255);h.store(0x405,c>>8)
    for a,v in [(0x800,0),(0x801,255),(0x804,0x40),(0x804,0),(0x805,255),(0x802,0x40),(0x803,1)]:h.store(a,v)
    for v in [0]*16+[31]*16:h.store(0x806,v)
    h.store(0x804,0x80);h.reg(5,0xc8)
    h.label('frame');h.addr(0xad,0);h.e(0x29,0x20);h.branch_zero('frame')
    # CLR/SEL reset, action nibble then direction nibble, active low.
    h.store(0x1000,3);h.store(0x1000,0);h.addr(0xad,0x1000);h.e(0x49,15,0x29,15);h.addr(0x8d,0x4011);h.addr(0x8d,0x2002)
    h.e(0x29,3);h.branch_zero('quiet');h.store(0x804,0x9f);h.jump('pad')
    h.label('quiet');h.store(0x804,0x80)
    h.label('pad');h.store(0x1000,3);h.addr(0xad,0x1000);h.e(0x49,15,0x29,15);h.addr(0x8d,0x4012);h.addr(0x8d,0x2003)
    for bit,a,op,label in [(2,0x2000,0xee,'right'),(8,0x2000,0xce,'left'),(4,0x2001,0xee,'down'),(1,0x2001,0xce,'up')]:
        h.addr(0xad,0x2003);h.e(0x29,bit);h.branch_zero(label);h.addr(op,a);h.label(label)
    h.vram(0x6000);h.addr(0xad,0x2001);h.e(0x18,0x69,64);h.addr(0x8d,2);h.e(0x23,0)
    h.addr(0xad,0x2000);h.addr(0x8d,0x4010);h.e(0x18,0x69,32);h.addr(0x8d,2);h.e(0x23,0)
    h.word(0x100);h.word(0x80);h.reg(19,0x6000)
    h.addr(0xee,0x4013);h.jump('frame')
    code=h.done();assert len(code)<0x1f00
    rom=bytearray(8192);rom[:len(code)]=code
    for at in range(0x1ff6,0x2000,2):struct.pack_into('<H',rom,at,0xe000)
    return rom

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output',type=Path,default=Path(__file__).resolve().parents[1]/'out/pkg-res/turbografx16/demo.pce')
    a=p.parse_args();a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_bytes(demo())
if __name__=='__main__':main()
