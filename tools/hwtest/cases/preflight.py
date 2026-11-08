"""Suite 0: what is on the other end of the cable, before anything changes it."""
import re

from ..runner import case, card_kind


@case('preflight', 'identity', covers='-')
def identity(ctx):
    """Device answers HELLO; records runtime, build and capabilities."""
    rc, out = ctx.dev.run('probe', timeout=30)
    assert ctx.dev.build, 'no build in probe output'
    ctx.note('%s %s, %d KiB free, caps 0x%x' % (ctx.dev.runtime, ctx.dev.build, ctx.dev.free_kib,
                                                 ctx.dev.capabilities))
    ctx.state['initial_build'] = ctx.dev.build
    assert ctx.dev.runtime == 'Glaze', 'device is running %s; boot Glaze first' % ctx.dev.runtime


@case('preflight', 'console', covers='-')
def console(ctx):
    """Drains the console; records the boot banner, battery and card lines."""
    ctx.dev.drain()
    text = ctx.dev.log_text()
    for rx in (r'^Glaze .* booting.*$', r'^battery: .*$', r'^romfs: .*$', r'^cf: (network )?card .*$',
               r'^power: awake.*$'):
        found = re.findall(rx, text, re.M)
        if found:
            ctx.note(found[-1])
    ctx.shot('screen')


@case('preflight', 'card', covers='-')
def card(ctx):
    """Identifies the card in the CF slot (none, wl54cf, nic, storage, gps)."""
    kind = card_kind(ctx.dev)
    ctx.state['card'] = kind
    ctx.note('card: %s' % kind)


@case('preflight', 'power', covers='-')
def power(ctx):
    """Battery band and adapter, when the kernel has GU_POWER."""
    if not ctx.dev.capabilities & 0x200000:
        ctx.note('no GU_POWER in this kernel')
        return
    rc, out = ctx.dev.run('power', 'status', timeout=30)
    first = out.strip().splitlines()[-2] if out.count('\n') > 1 else out
    ctx.note(first)
    m = re.search(r'percent=(\d+)', out)
    f = re.search(r'flags=0x([0-9a-f]+)', out)
    ctx.state['battery'] = int(m.group(1)) if m else None
    ctx.state['power_flags'] = int(f.group(1), 16) if f else None
