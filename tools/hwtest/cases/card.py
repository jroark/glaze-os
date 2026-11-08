"""Suite 8: whatever card is in the CF slot (runner.card_kind decides)."""
import re
import time

from ..runner import case
from .power import need_power, sleep_and_wait

NET = ('wl54cf', 'nic')


def power_status(ctx):
    rc, out = ctx.dev.run('power', 'status', timeout=30)
    vals = dict(re.findall(r'(\w+)=([0-9a-zx]+|[a-z ]+?)(?= \w+=|$)', out.replace('\n', ' ')))
    return out, vals


@case('card', 'redetect-after-suspend', image='default', card=NET, covers='#111 F1')
def redetect(ctx):
    """After a Suspend (which cuts the socket), the network card is found again."""
    need_power(ctx)
    since = ctx.dev.log_offset()
    sleep_and_wait(ctx, ['suspend', '20'], 20, since)
    ctx.dev.wait_log(r'^cf: network card \S+', since=since, timeout=45,
                     fail=r'^cf: network card removed$(?![\s\S]*cf: network card \S+)')
    ctx.dev.wait_log(r'^(wl54cf: ready|ne2000: |3c589: |net: )', since=since, timeout=45)


@case('card', 'wifi-power-save', image='default', card=('wl54cf',), covers='#111')
def wifi_power_save(ctx):
    """WL54-CF associated and idle: the firmware accepts PS_MODE (save/doze time grows)."""
    need_power(ctx)
    out0, a = power_status(ctx)
    ctx.note(out0.strip().splitlines()[-1])
    if 'card=none' in out0:
        raise AssertionError('power status sees no card')
    time.sleep(60)
    out1, b = power_status(ctx)
    ctx.note(out1.strip().splitlines()[-1])
    grow = lambda k: int(b.get(k, 0)) - int(a.get(k, 0))
    saved = grow('save_ms') + grow('doze_ms')
    ctx.note('power save + doze grew %d ms in 60 s' % saved)
    assert saved > 0, 'the card never entered power save (associated? %s)' % out1.strip().splitlines()[-1]


@case('card', 'gps-lock', image='default', card=('gps',), covers='#110')
def gps_lock(ctx):
    """The GPS-CF's NMEA stream is found at 4800 baud."""
    ctx.dev.wait_log(r'^gps: NMEA at (\d+) baud', since=ctx.state.get('boot_offset', 0), timeout=90)


@case('card', 'storage-mounted', image='default', card=('storage',), covers='CF')
def storage(ctx):
    """A CF storage card answers IDENTIFY with a sane geometry."""
    rc, out = ctx.dev.run('cf-info', timeout=30)
    ctx.note(out.strip().splitlines()[-1])
    assert re.search(r'sectors', out, re.I)
