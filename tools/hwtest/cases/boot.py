"""Suite 2: the default RAM image (LZ4 romfs, WL54-CF firmware) boots."""
import re

from ..runner import case


@case('boot', 'lz4-springboard', image='default', covers='#114 #116')
def lz4_springboard(ctx):
    """LZ4 romfs inflates on the device and SpringBoard comes up."""
    since = ctx.state.get('boot_offset', 0)
    ctx.dev.wait_log(r'^springboard: ready', since=since, timeout=120)
    text = ctx.dev.log_text(since)
    m = re.search(r'^romfs: (.*)$', text, re.M)
    assert m and 'LZ4' in m.group(1), 'romfs line: %s' % (m.group(1) if m else 'missing')
    ctx.note('romfs: ' + m.group(1))
    assert not re.search(r'bad LZ4|lz4.*(error|fail)', text, re.M | re.I), 'LZ4 decode error in the log'
    r = re.search(r'^sb: ready (\d+) ms free (\d+) KiB', text, re.M)
    if r:
        ctx.note('SpringBoard ready %s ms after start, %s KiB free' % r.groups())
    ctx.note('reload + reconnect %.1f s' % ctx.state.get('reload_s', 0))
    ctx.shot('lock')


@case('boot', 'unlock', image='default', covers='core')
def unlock(ctx):
    """Slide to unlock reaches the home screen."""
    ctx.dev.script_text('500 drag 40 280 235 280 500', ctx.path('home.png'), name='unlock')
    ctx.evidence.append('cases/%s/home.png' % ctx.case_id)
