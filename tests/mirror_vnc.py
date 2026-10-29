#!/usr/bin/env python3
"""Optional independent-client interoperability check (asyncvnc==1.3.0).
Build out/host/mirror-fixture with `make -C host mirror-test` first. The
ordinary mirror-test target uses only the Python standard library.
"""
import asyncio
import asyncvnc
from mirror import bridge,metrics

async def verify(port):
    async with asyncvnc.connect('127.0.0.1',port) as client:
        pixels=await asyncio.wait_for(client.screenshot(),5)
        assert pixels.shape==(320,240,4)
        assert pixels[0,:3].tolist()==[[255,0,0,255],[0,255,0,255],[0,0,255,255]]
        client.mouse.move(5,5);client.mouse.click();client.keyboard.write('a')
        await client.drain()
        pixels=await asyncio.wait_for(client.screenshot(),5)
        assert pixels[5,5].tolist()==[255,255,255,255]
        client.mouse.move(6,5);client.mouse.click();client.video.refresh()
        await asyncio.wait_for(client.read(),5)
        assert client.video.as_rgba()[5,6].tolist()==[255,255,255,255]

with bridge() as (initial,path,port):
    initial.close()
    asyncio.run(verify(port))
    values,data=metrics(path)
    assert values[1]==1 and values[2]<=300*516+2*516
    assert data
print('PASS: independent asyncvnc client, full/incremental RGB, mouse, text and sparse upstream deltas')
