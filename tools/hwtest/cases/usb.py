"""Suite 7: the USB debug link surviving resets and vanishing hosts. A real
cable pull is manual/usb-unplug; a bus reset drops the device's session
through the same GZ_USB_STATE change (servers/usb/main.c)."""
import hashlib
import os
import signal
import subprocess
import time

from ..device import ROOT
from ..runner import case


def blob(ctx, size):
    path = os.path.join(ctx.rundir, 'blob-%d.bin' % size)
    if not os.path.exists(path):
        with open(path, 'wb') as f:
            f.write(os.urandom(size))
    return path


def sha(path):
    return hashlib.sha256(open(path, 'rb').read()).hexdigest()


def roundtrip(ctx, local, remote):
    back = ctx.path('back.bin')
    if os.path.exists(back):
        os.remove(back)
    ctx.dev.put(local, remote)
    ctx.dev.get(remote, back)
    assert sha(back) == sha(local), 'file changed in transit'


def interrupted(ctx, args, after_s):
    """Starts a glaze-usb command and kills it after_s seconds in, as a host
    that vanishes mid-transfer. Holds the device lock meanwhile."""
    with ctx.dev.lock:
        p = subprocess.Popen(ctx.dev._argv(args), stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        time.sleep(after_s)
        p.send_signal(signal.SIGKILL)
        p.wait()


@case('usb', 'bus-reset-idle', image='default', covers='USB')
def bus_reset_idle(ctx):
    """A USB bus reset while idle: the device re-enumerates, HELLO works."""
    since = ctx.dev.log_offset()
    ctx.dev.run('reset-link', timeout=30, check=False)
    took = ctx.dev.wait_device(60)
    ctx.note('answered %.1f s after the reset' % took)
    blobp = blob(ctx, 64 * 1024)
    roundtrip(ctx, blobp, '/Documents/hwtest-64k.bin')
    ctx.check_no_bad_lines(since)


@case('usb', 'host-killed-mid-put', image='default', covers='USB')
def killed_put(ctx):
    """The host dies mid-upload: the device drops the transfer, a retry
    succeeds and the file reads back intact."""
    big = blob(ctx, 1024 * 1024)
    interrupted(ctx, ['put', big, '/Documents/hwtest-1m.bin'], 4)
    took = ctx.dev.wait_device(60)
    ctx.note('answered %.1f s after the kill' % took)
    roundtrip(ctx, big, '/Documents/hwtest-1m.bin')


@case('usb', 'reset-mid-put', image='default', covers='USB')
def reset_put(ctx):
    """A bus reset mid-upload (the host's view of a cable glitch)."""
    big = blob(ctx, 1024 * 1024)
    with ctx.dev.lock:
        p = subprocess.Popen(ctx.dev._argv(['put', big, '/Documents/hwtest-1m.bin']), stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT)
        time.sleep(4)
        p.send_signal(signal.SIGKILL)
        p.wait()
        subprocess.run(ctx.dev._argv(['reset-link']), capture_output=True, timeout=30)
    ctx.dev.wait_device(60)
    roundtrip(ctx, big, '/Documents/hwtest-1m.bin')


@case('usb', 'host-killed-mid-logs', image='default', covers='USB')
def killed_logs(ctx):
    """The host dies while streaming logs; the next session still gets logs."""
    interrupted(ctx, ['logs'], 5)
    ctx.dev.wait_device(60)
    since = ctx.dev.log_offset()
    ctx.dev.script_text('500 drag 40 280 235 280 500', name='unlock')    # run needs it unlocked
    ctx.dev.launch('bin/settings')
    ctx.dev.wait_log(r'proc: spawned settings|settings', since=since, timeout=30)


@case('usb', 'script-cancel', image='default', covers='USB input')
def script_cancel(ctx):
    """A long script cut off by the host releases held input."""
    path = os.path.join(ctx.rundir, 'scripts', 'long-hold.txt')
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w') as f:
        f.write('300 down 120 160\n20000 up 120 160\n')
    interrupted(ctx, ['script', path], 3)
    ctx.dev.wait_device(60)
    # Input works afterwards: Home returns to the home screen.
    ctx.dev.script_text('300 key home\n1500 key home', ctx.path('after.png'), name='after-cancel')
    ctx.evidence.append('cases/%s/after.png' % ctx.case_id)


@case('usb', 'dead-link', image='default', covers='USB')
def dead_link(ctx):
    """The device's USB unit goes dead for 5 s (GU_USB_DETACH): the reply
    arrives first, the host's transfers fail meanwhile, and the link comes
    back without a reload. (The BE-300 keeps its pull-up, so this is not an
    unplug; see manual/usb-unplug.)"""
    ctx.dev.probe()
    if not ctx.dev.capabilities & 0x400000:
        from ..runner import Skip
        raise Skip('kernel has no GU_USB_DETACH')
    since = ctx.dev.log_offset()
    ctx.dev.run('usb-detach', '5000', timeout=30)
    time.sleep(1.5)
    rc, out = ctx.dev.run('probe', timeout=15, retries=0, check=False)
    ctx.note('probe while dead: rc=%d' % rc)
    took = ctx.dev.wait_device(60)
    ctx.note('answered again %.1f s later (after a host bus reset: the unit came back without its address)' % took)
    ctx.dev.wait_log(r'^usb: back on the bus', since=since, timeout=30)
    roundtrip(ctx, blob(ctx, 256 * 1024), '/Documents/hwtest-256k.bin')
