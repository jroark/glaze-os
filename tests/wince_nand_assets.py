#!/usr/bin/env python3
"""Synthetic NK/CPK contracts for the native decoder; no ROMs are checked in."""
from pathlib import Path
import os
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
CLI=ROOT/'out/host/wince-assets-native'
def put(b,off,value):struct.pack_into('<I',b,off,value&0xffffffff)
def literal(data):return b''.join(b'\xff'+data[i:i+8] for i in range(0,len(data),8))
def synthetic_nk():
    """Small standard ROMHDR with two export modules and an original dictionary."""
    base=0x80060000;flat=bytearray(0x6000)
    put(flat,0x40,0x43454345);put(flat,0x44,base+0x100);put(flat,0x110,3)
    for i,name in enumerate(('coredll.dll','commctrl.dll','CCFS.DLL')):
        entry=0x150+i*32;meta=0x300+i*0x80;raw=0x1000+i*0x1000
        flat[meta:meta+len(name)+1]=name.encode()+b'\0';put(flat,entry+20,base+meta)
        put(flat,entry+24,base+meta+32);put(flat,entry+28,base+meta+56)
        struct.pack_into('<H',flat,meta+32,1);put(flat,meta+52,0x2000)
        for at,v in enumerate((0x1000,0x1000,0x1000,base+raw,0,0x60000020)):put(flat,meta+56+at*4,v)
        if i<2:
            flat[raw+0x100:raw+0x100+len(name)+1]=name.encode()+b'\0'
            for at,v in enumerate((0,0,0,0x1100,1,1,1,0x1180,0x1184,0x1188)):put(flat,raw+at*4,v)
            put(flat,raw+0x180,0x1400);put(flat,raw+0x184,0x1200)
            struct.pack_into('<H',flat,raw+0x188,0)
            symbol=('GetTickCount' if i==0 else 'InitCommonControls').encode()+b'\0';flat[raw+0x200:raw+0x200+len(symbol)]=symbol
        else:
            # A complete small four-bit dictionary, not copied from a ROM.
            t=raw+0x100;put(flat,t,0xffff);put(flat,t+4+5*4,-2);put(flat,t+4+6*4,-7)
            put(flat,t+0x84+4*4,2);struct.pack_into('<HH',flat,t+0x104,0x1234,0x5678)
    stream=b'B000FF\n'+struct.pack('<II',base,len(flat))+struct.pack('<III',base,len(flat),sum(flat)&0xffffffff)+flat+struct.pack('<III',0,base,0)
    return literal(stream)
def nand_fixture(packed):
    count=(len(packed)+511)//512;out=bytearray(512+count*512)
    put(out,40,1);put(out,44,count);out[512:512+len(packed)]=packed;return out

def cpk(kind,payload,size=512):
    b=bytearray(56);b[:12]=b'CASIO.$&%-Z\1';put(b,44,size);put(b,48,1);put(b,52,kind<<24);return b+payload

def constant_run(kind):
    nibble=kind>=4;unit=4 if nibble else 8;threshold=kind-2 if nibble else kind+1
    remaining=1024 if nibble else 512;values=[];run=0;value=0xa if nibble else 0xab
    while remaining:
        values.append(value);remaining-=1;run+=1
        if run==threshold:
            repeat=min((1<<unit)-1,remaining);values.append(repeat);remaining-=repeat;run=0
    if nibble:return bytes(values[i]|(values[i+1]<<4 if i+1<len(values) else 0) for i in range(0,len(values),2))
    return bytes(values)

class Assets(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.path=Path(self.tmp.name)
        self.nand=self.path/'nand.bin';self.names=self.path/'ordinals.txt';self.dictionary=self.path/'dictionary.bin'
        self.nand.write_bytes(nand_fixture(synthetic_nk()))
        self.run_cli('recover',self.nand,self.names,self.dictionary)
    def tearDown(self):self.tmp.cleanup()
    def run_cli(self,*args,success=True):
        p=subprocess.run([str(CLI),*map(str,args)],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        self.assertEqual(p.returncode==0,success,p.stderr.decode());return p
    def decode(self,kind,payload,expected):
        source=self.path/'source.cpk';target=self.path/'target.exe';source.write_bytes(cpk(kind,payload))
        self.run_cli('decode',source,self.dictionary,target);self.assertEqual(target.read_bytes(),expected)
    def test_exports(self):
        self.assertEqual(self.names.read_text(),'coredll.dll 1 GetTickCount\ncommctrl.dll 1 InitCommonControls\n')
        self.assertEqual(len(self.dictionary.read_bytes()),0x108)
    def test_all_block_kinds(self):
        self.decode(0,b'X'*512,b'X'*512)
        for kind in range(1,9):self.decode(kind,constant_run(kind),bytes([0xaa if kind>=4 else 0xab])*512)
        for kind in range(9,15):
            nibble=kind>=13;width=kind-(12 if nibble else 8);unit=4 if nibble else 8
            start=((1<<width)-1)*unit;bits=(1024 if nibble else 512)*width
            raw=bytearray((start+bits+7)//8);raw[0]=0xa if nibble else 0xab
            self.decode(kind,raw,bytes([0xaa if nibble else 0xab])*512)
        self.decode(15,bytes(128),b'\x34\x12'*256)
        self.decode(16,literal(b'Z'*512),b'Z'*512)
    def test_reject_bad_bounds(self):
        b=nand_fixture(synthetic_nk());put(b,44,0xffffffff);self.nand.write_bytes(b)
        self.run_cli('recover',self.nand,self.names,self.dictionary,success=False)
        source=self.path/'bad.cpk';target=self.path/'out';source.write_bytes(cpk(0,b'a'*512));b=bytearray(source.read_bytes());put(b,52,0xffffff);source.write_bytes(b)
        self.run_cli('decode',source,self.dictionary,target,success=False)
        source.write_bytes(cpk(15,b''));self.run_cli('decode',source,self.dictionary,target,success=False)
        source.write_bytes(cpk(16,b'\x00'));self.run_cli('decode',source,self.dictionary,target,success=False)
    def test_optional_local_nand(self):
        image=os.environ.get('WINCE_NAND')
        if not image:self.skipTest('set WINCE_NAND for a locally supplied original NAND')
        self.nand.write_bytes(Path(image).read_bytes());self.run_cli('recover',self.nand,self.names,self.dictionary)
        sys.path.insert(0,str(ROOT/'tools'));from wince_assets import Fat16,CpkDecoder
        volume=Fat16(self.nand.read_bytes());decoder=CpkDecoder.__new__(CpkDecoder);decoder.driver=self.dictionary.read_bytes();decoder.table=0
        for name in ('calc.exe','LC.dll','Calendar.exe'):
            data=volume.read('Program Files/'+name+'.cpk');source=self.path/(name+'.cpk');target=self.path/name;source.write_bytes(data)
            self.run_cli('decode',source,self.dictionary,target)
            self.assertEqual(target.read_bytes(),decoder.unpack(data))
            self.assertTrue(target.read_bytes().startswith(b'MZ'))

if __name__=='__main__':unittest.main()
