#!/usr/bin/env python3
"""Original CC0 controller/graphics/audio Sega demos. ROM outputs stay under out/."""
from pathlib import Path
import argparse
import struct

class Z80:
    def __init__(self): self.data=bytearray(); self.labels={}; self.fix=[]
    def e(self,*v): self.data.extend(v)
    def l(self,n): self.labels[n]=0x100+len(self.data)
    def j(self,op,n): self.e(op,0,0); self.fix.append((len(self.data)-2,n))
    def a(self,v): self.e(0x3e,v)
    def write(self,addr): self.e(0x32,addr&255,addr>>8)
    def read(self,addr): self.e(0x3a,addr&255,addr>>8)
    def out(self,port,v): self.a(v); self.e(0xd3,port)
    def addr(self,a,kind=0x40): self.out(0xbf,a&255); self.out(0xbf,kind|(a>>8))
    def done(self):
        for at,n in self.fix: struct.pack_into('<H',self.data,at,self.labels[n])
        return self.data

def sms():
    z=Z80();z.e(0xf3,0x31,0xf0,0xdf);z.a(8);z.write(0xfffc)
    z.read(0x8000);z.e(0xfe,8);z.j(0xd2,'pos');z.a(120)
    z.l('pos');z.write(0xc000);z.a(80);z.write(0xc001)
    for i,v in enumerate([4,0,14,0xff,0,0x7f,0,0,0,0,0xff]):z.out(0xbf,v);z.out(0xbf,0x80|i)
    z.addr(0);z.e(0x01,0,0x40)
    z.l('clear');z.e(0xaf,0xd3,0xbe,0x0b,0x78,0xb1);z.j(0xc2,'clear')
    z.addr(32)
    tile=bytes([0xf0,0xcc,0xaa,0]*8)
    sprite=bytes(v for mask in [0x18,0x3c,0x7e,0xff,0xff,0x7e,0x3c,0x18] for v in [mask,mask,mask,mask])
    for v in tile+sprite:z.out(0xbe,v)
    z.addr(0,0xc0)
    palette=[0,3,12,48,63,15,51,60]+[0]*8+[0,3,12,48,63,15,51,60]+[63]*8
    for v in palette:z.out(0xbe,v)
    z.addr(0x3800);z.e(0x01,0xc0,3)
    z.l('map');z.out(0xbe,1);z.out(0xbe,0);z.e(0x0b,0x78,0xb1);z.j(0xc2,'map')
    z.addr(0x3f00);z.out(0xbe,79);z.out(0xbe,208)
    z.addr(0x3f80);z.out(0xbe,120);z.out(0xbe,2)
    z.out(0xbf,0x40);z.out(0xbf,0x81)
    z.out(0x7f,0x80);z.out(0x7f,0x08);z.out(0x7f,0x9f)
    z.l('frame');z.e(0xdb,0xbf,0xe6,0x80);z.j(0xca,'frame')
    z.e(0xdb,0xdc,0x2f,0xe6,0x3f);z.write(0x8001);z.e(0x47)
    for bit,addr,op,label in [(3,0xc000,0x3c,'r'),(2,0xc000,0x3d,'l'),(1,0xc001,0x3c,'d'),(0,0xc001,0x3d,'u')]:
        z.e(0xcb,0x40+8*bit);z.j(0xca,label);z.read(addr);z.e(op);z.write(addr);z.l(label)
    z.e(0x78,0xe6,0x30);z.j(0xca,'quiet');z.out(0x7f,0x90);z.j(0xc3,'draw')
    z.l('quiet');z.out(0x7f,0x9f)
    z.l('draw');z.addr(0x3f80);z.read(0xc000);z.write(0x8000);z.e(0xd3,0xbe)
    z.addr(0x3f00);z.read(0xc001);z.e(0xd3,0xbe)
    z.read(0x8003);z.e(0x3c);z.write(0x8003);z.j(0xc3,'frame')
    rom=bytearray(32768);rom[:3]=b'\xc3\x00\x01';rom[0x66:0x6e]=bytes([0xe5,0x21,4,0x80,0x34,0xe1,0xed,0x45])
    data=z.done();assert len(data)<0x3ef0;rom[0x100:0x100+len(data)]=data
    rom[0x7ff0:0x7ff8]=b'TMR SEGA';rom[0x7fff]=0x4c
    return rom

class M68K:
    def __init__(self): self.data=bytearray();self.labels={};self.fix=[]
    def w(self,v):self.data+=struct.pack('>H',v&65535)
    def d(self,v):self.data+=struct.pack('>I',v&0xffffffff)
    def l(self,n):self.labels[n]=0x200+len(self.data)
    def branch(self,op,n):self.w(op);self.fix.append((len(self.data),n));self.w(0)
    def byte(self,a,v):self.w(0x13fc);self.w(v);self.d(a)
    def word(self,a,v):self.w(0x33fc);self.w(v);self.d(a)
    def long(self,a,v):self.w(0x23fc);self.d(v);self.d(a)
    def readw(self,a,reg=0):self.w(0x3039|(reg<<9));self.d(a)
    def spriteword(self,a):
        self.readw(a,1);self.w(0x0641);self.w(128);self.w(0x33c1);self.d(0xc00000)
    def done(self):
        for at,n in self.fix:struct.pack_into('>h',self.data,at,self.labels[n]-(0x200+at))
        return self.data

def vram(a):return 0x40000000|((a&0x3fff)<<16)|((a>>14)&3)
def genesis():
    m=M68K();m.w(0x46fc);m.w(0x2700)
    # Divide-by-zero and handled ILLEGAL exceptions exercise the CPU dispatcher,
    # including its MIPS setjmp/longjmp backend.
    m.w(0x7000);m.w(0x80fc);m.w(0);m.w(0x4afc)
    for i,v in enumerate([4,4,0x30,0x2c,7,0x78,0,0,0,0,0xff,0,0x81,0,0,2,1,0,0,0,0,0,0]):m.word(0xc00004,0x8000|(i<<8)|v)
    m.long(0xc00004,vram(0));m.w(0x7000);m.w(0x323c);m.w(32767)
    m.l('clear');m.w(0x33c0);m.d(0xc00000);m.branch(0x51c9,'clear')
    m.long(0xc00004,0xc0000000)
    for v in [0,0x00e,0x0e0,0xe00,0xeee,0x0ee,0xee0,0xe0e]:m.word(0xc00000,v)
    m.long(0xc00004,vram(32))
    for v in [0x12341234]*8+[0x00044000,0x00411400,0x04111140,0x41111114,0x41111114,0x04111140,0x00411400,0x00044000]:m.long(0xc00000,v)
    m.long(0xc00004,vram(0xc000));m.w(0x7001);m.w(0x323c);m.w(2047)
    m.l('map');m.w(0x33c0);m.d(0xc00000);m.branch(0x51c9,'map')
    m.word(0xff0000,120);m.word(0xff0002,80);m.byte(0xa10009,0x40)
    m.word(0xa11100,0x0100);m.word(0xa11200,0x0100)
    # A simple four-operator FM tone, alongside the controller-driven PSG.
    for reg,value in [(0x30,1),(0x34,1),(0x38,1),(0x3c,1),(0x40,0x20),(0x44,0x20),(0x48,0x20),(0x4c,0x20),(0x50,0x1f),(0x54,0x1f),(0x58,0x1f),(0x5c,0x1f),(0x80,0x0f),(0x84,0x0f),(0x88,0x0f),(0x8c,0x0f),(0xb0,7),(0xb4,0xc0),(0xa4,0x22),(0xa0,0x69),(0x28,0xf0)]:
        m.byte(0xa04000,reg);m.byte(0xa04001,value)
    for v in [0x80,8,0x9f]:m.byte(0xc00011,v)
    m.word(0xc00004,0x8144)
    m.l('frame');m.readw(0xc00004);m.w(0x0240);m.w(8);m.branch(0x6700,'frame')
    m.byte(0xa10003,0x40);m.w(0x1039);m.d(0xa10003);m.w(0x4600);m.w(0x0200);m.w(0x3f)
    m.w(0x13c0);m.d(0x200001)
    for bit,addr,opcode,label in [(3,0xff0000,0x5279,'right'),(2,0xff0000,0x5379,'left'),(1,0xff0002,0x5279,'down'),(0,0xff0002,0x5379,'up')]:
        m.w(0x0800);m.w(bit);m.branch(0x6700,label);m.w(opcode);m.d(addr);m.l(label)
    m.w(0x0800);m.w(4);m.branch(0x6700,'quiet');m.byte(0xc00011,0x90);m.branch(0x6000,'draw')
    m.l('quiet');m.byte(0xc00011,0x9f)
    m.l('draw');m.byte(0xa10003,0);m.w(0x1039);m.d(0xa10003);m.w(0x4600);m.w(0x0200);m.w(0x30);m.w(0x13c0);m.d(0x200003)
    m.w(0x5239);m.d(0x200005)
    m.long(0xc00004,vram(0xf000));m.spriteword(0xff0002);m.word(0xc00000,0);m.word(0xc00000,2);m.spriteword(0xff0000)
    m.l('wait');m.readw(0xc00004);m.w(0x0240);m.w(8);m.branch(0x6600,'wait');m.branch(0x6000,'frame')
    rom=bytearray(65536);struct.pack_into('>II',rom,0,0x00fffffc,0x200)
    struct.pack_into('>I',rom,5*4,0x180);struct.pack_into('>H',rom,0x180,0x4e73)
    struct.pack_into('>I',rom,4*4,0x190);struct.pack_into('>HHH',rom,0x190,0x54af,2,0x4e73)
    rom[0x100:0x110]=b'SEGA MEGA DRIVE ';rom[0x150:0x15a]=b'GLAZE DEMO';rom[0x1f0:0x1f3]=b'JUE'
    rom[0x1b0:0x1b4]=b'RA\xf8\x20';struct.pack_into('>II',rom,0x1b4,0x200001,0x203fff)
    struct.pack_into('>II',rom,0x1a0,0,65535);struct.pack_into('>II',rom,0x1a8,0xff0000,0xffffff)
    data=m.done();assert len(data)<32768;rom[0x200:0x200+len(data)]=data
    struct.pack_into('>H',rom,0x18e,sum(struct.unpack('>'+str((len(rom)-512)//2)+'H',rom[512:]))&65535)
    return rom

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('system',choices=['sms','genesis']);p.add_argument('--output',type=Path)
    args=p.parse_args();name='mastersystem' if args.system=='sms' else 'genesis';ext='sms' if args.system=='sms' else 'gen'
    out=args.output or Path(__file__).resolve().parents[1]/f'out/pkg-res/{name}/demo.{ext}'
    out.parent.mkdir(parents=True,exist_ok=True);out.write_bytes(sms() if args.system=='sms' else genesis())
if __name__=='__main__':main()
