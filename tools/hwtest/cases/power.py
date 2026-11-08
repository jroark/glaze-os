"""Suite 6: power off and wake without hands, through `glaze-usb power`
(an RTC alarm armed by the debugger) on the default RAM image."""
import re
import time

import numpy as np

from .. import compare
from ..runner import case, Skip

UNLOCK = '500 drag 40 280 235 280 500'


def need_power(ctx):
    ctx.dev.probe()
    if not ctx.dev.capabilities & 0x200000:
        raise Skip('kernel has no GU_POWER (reload a newer image)')


def sleep_and_wait(ctx, args, expect_s, since, extra=60):
    """Runs a power command, waits for USB to come back and for the wake line."""
    t0 = time.time()
    ctx.dev.pump_paused.set()
    try:
        ctx.dev.run('power', *args, timeout=30)
        time.sleep(max(3, expect_s - 5))
        back = ctx.dev.wait_device(timeout=extra + 30)
    finally:
        ctx.dev.pump_paused.clear()
    m = ctx.dev.wait_log(r'^power: awake \((\w+)\) after (\d+) s', since=since, timeout=30)
    took = time.time() - t0
    ctx.note('awake by %s after %s s; USB answered %.0f s after the command' % (m.group(1), m.group(2), took))
    return m, took


def front_app_survives(ctx, shot_name):
    """Settings in front before the sleep is in front after unlocking."""
    ctx.dev.launch('bin/settings')
    time.sleep(3)
    return ctx.shot(shot_name)


def same_screen(a, b, limit=0.02):
    x = compare.read_png(a).astype(int)[16:]
    y = compare.read_png(b).astype(int)[16:]
    return (np.abs(x - y).max(axis=2) > 40).mean() <= limit


@case('power', 'suspend-alarm', image='default', covers='POWER #111')
def suspend_alarm(ctx):
    """Suspend 30 s, alarm wake, USB back, the front app resumes."""
    need_power(ctx)
    ctx.dev.script_text(UNLOCK, name='unlock')
    before = front_app_survives(ctx, 'before')
    since = ctx.dev.log_offset()
    m, _ = sleep_and_wait(ctx, ['suspend', '30'], 30, since)
    assert m.group(1) == 'alarm', 'woke by %s' % m.group(1)
    assert 25 <= int(m.group(2)) <= 40, 'slept %s s' % m.group(2)
    text = ctx.dev.log_text(since)
    assert 'springboard: power off, debugger (Suspend)' in text
    assert not re.search(r'^Glaze .* booting', text, re.M), 'the device restarted'
    ctx.dev.script_text(UNLOCK, ctx.path('after.png'), name='unlock-after')
    ctx.evidence.append('cases/%s/after.png' % ctx.case_id)
    assert same_screen(before, ctx.path('after.png')), 'Settings is not in front after the wake'


@case('power', 'suspend-past-watchdog', image='default', covers='POWER cmcu')
def suspend_watchdog(ctx):
    """Suspend 200 s, past the CMCU's 150 s watchdog: no reset."""
    need_power(ctx)
    since = ctx.dev.log_offset()
    m, _ = sleep_and_wait(ctx, ['suspend', '200'], 200, since)
    assert m.group(1) == 'alarm' and int(m.group(2)) >= 190, 'woke by %s after %s s' % m.groups()
    assert not re.search(r'^Glaze .* booting', ctx.dev.log_text(since), re.M), 'watchdog reset the device'


@case('power', 'power-key-hold', image='default', covers='POWER')
def power_key(ctx):
    """Holding Power shows Goodbye and suspends; the armed alarm wakes it."""
    need_power(ctx)
    since = ctx.dev.log_offset()
    # Arm only the alarm: an idle timeout this long never fires first.
    ctx.dev.run('power', 'idle', '40', '600000', timeout=30)
    ctx.dev.pump_paused.set()
    try:
        ctx.dev.script_text('300 key power 2000', name='power-hold')
        time.sleep(35)
        ctx.dev.wait_device(timeout=90)
    finally:
        ctx.dev.pump_paused.clear()
    m = ctx.dev.wait_log(r'^power: awake \((\w+)\) after (\d+) s', since=since, timeout=30)
    text = ctx.dev.log_text(since)
    assert 'power off, Power held' in text, 'no Power-held power off'
    assert m.group(1) == 'alarm', 'woke by %s' % m.group(1)
    ctx.note('awake by %s after %s s' % m.groups())


@case('power', 'idle-off', image='default', covers='POWER')
def idle_off(ctx):
    """The idle power off runs once no debugger holds it off; alarm wakes it."""
    need_power(ctx)
    since = ctx.dev.log_offset()
    ctx.dev.pump_paused.set()
    try:
        ctx.dev.run('power', 'idle', '75', '8000', timeout=30)
        # No USB requests now: the session drops, the hold-off ends, the
        # screen goes off after 4 s and the device powers off 4 s later.
        time.sleep(80)
        ctx.dev.wait_device(timeout=90)
    finally:
        ctx.dev.pump_paused.clear()
    m = ctx.dev.wait_log(r'^power: awake \((\w+)\) after (\d+) s', since=since, timeout=30)
    text = ctx.dev.log_text(since)
    assert 'springboard: power off, idle (Suspend)' in text, 'no idle power off'
    assert m.group(1) == 'alarm', 'woke by %s' % m.group(1)
    ctx.note('awake by %s after %s s' % m.groups())


@case('power', 'status', image='default', covers='POWER #111')
def status(ctx):
    """Battery, adapter and card power through `power status`."""
    need_power(ctx)
    rc, out = ctx.dev.run('power', 'status', timeout=30)
    for line in out.splitlines():
        if line.startswith(('flags=', 'card=')):
            ctx.note(line)
    m = re.search(r'percent=(\d+)', out)
    assert m and int(m.group(1)) in (10, 25, 50, 75, 100), 'battery band %s' % (m and m.group(1))
