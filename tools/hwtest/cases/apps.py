"""Suite 4: installable apps (glaze-apps out/pkg and this repo's packages)
installed over USB, launched, poked and closed on the device."""
import os
import re
import time

import numpy as np

from .. import compare
from ..device import ROOT
from ..runner import case, Skip

APPS_DIR = os.path.join(os.path.dirname(ROOT), 'glaze-apps', 'out', 'pkg')
# Apps whose launch needs something the device can't have in this suite.
NEEDS = {'doom': 'a WAD', 'demo': None, 'catch': None}
# Light input after launch: taps in the middle and a swipe, then Home.
POKE = '''
500 tap 120 160
1500 tap 120 220
2500 drag 60 200 180 200 300
3500 key ok
'''


def unlocked(ctx):
    if ctx.state.get('apps_unlocked') == ctx.state.get('image'):
        return
    ctx.dev.wait_log(r'^springboard: ready', since=ctx.state['boot_offset'], timeout=120)
    ctx.dev.script_text('500 drag 40 280 235 280 500', name='unlock')
    ctx.state['apps_unlocked'] = ctx.state.get('image')


def app_case(ctx, name, pkg):
    if not os.path.exists(pkg):
        raise Skip('%s not built' % pkg)
    unlocked(ctx)
    since = ctx.dev.log_offset()
    ctx.dev.put(pkg, '/Downloads/%s.gzp' % name)
    ctx.dev.launch('/Downloads/%s.gzp' % name)
    time.sleep(6)
    first = ctx.shot('launched')
    ctx.dev.script_text(POKE, ctx.path('poked.png'), name='poke-' + name)
    ctx.evidence.append('cases/%s/poked.png' % ctx.case_id)
    text = ctx.dev.log_text(since)
    # Only this app's own process counts: SpringBoard keeps four apps and
    # ends the oldest with -9 when a fifth starts (SB_MAX_RUNNING).
    m = re.search(r'^(fault: .* in %s .*|proc: %s pid \d+ exited \((?!0\)).*\)|.*out of memory.*)$'
                  % (re.escape(name), re.escape(name)), text, re.M | re.I)
    assert not m, 'console: %s' % m.group(0)
    img = compare.read_png(first).astype(int)
    assert img[16:].max() > 40, 'launch screen is black'
    ctx.dev.script_text('300 key home', ctx.path('home.png'), name='home')
    ctx.evidence.append('cases/%s/home.png' % ctx.case_id)
    home = compare.read_png(ctx.path('home.png')).astype(int)
    assert np.abs(home[16:] - img[16:]).max(axis=2).mean() > 3, 'Home did not leave the app'


def register(name, pkg, covers):
    @case('apps', name, image='default', covers=covers)
    def fn(ctx):
        if NEEDS.get(name):
            raise Skip('needs %s' % NEEDS[name])
        app_case(ctx, name, pkg)
    fn.__doc__ = 'Install %s over USB, launch, poke, Home.' % name


for n in ('hello', 'maintenance'):
    register(n, os.path.join(ROOT, 'out', 'pkg', n + '.gzp'), 'packages')
for n in ('chess', 'twixt', 'snake', 'flappy', 'tetris', 'breakout', 'maps', 'minesweeper', 'solitaire',
          'fifteen', 'sketch', 'somafm', 'aim', 'gmail', 'chatgpt', 'doom'):
    register(n, os.path.join(APPS_DIR, n + '.gzp'), 'glaze-apps')
