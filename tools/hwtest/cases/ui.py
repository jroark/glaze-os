"""Suite 3: UI scripts on the real touch path's input layer, compared with the
emulator. Each case starts from a fresh RAM boot on the lock screen, as
tests/run.sh does, so the scripts' timings are the emulator's."""
import os
import re
import subprocess

from .. import compare
from ..device import ROOT
from ..runner import case, Skip

THRESHOLD = 0.02   # fraction of differing pixels allowed outside the masks


def fresh(ctx):
    ctx.state['image'] = None
    ctx.ensure_image('default')
    ctx.dev.wait_log(r'^springboard: ready', since=ctx.state['boot_offset'], timeout=120)


def scripted(ctx, script, seconds, masks=((0, 0, 240, 16),), env=None, hw_script=None, before=None):
    """Runs a tests/ui script on the device and in the emulator; compares the
    final screens."""
    ref = compare.emulator_ref(script, seconds, env=env, log=ctx.log)
    fresh(ctx)
    if before:
        before(ctx)
    hw = ctx.path('hw.png')
    ctx.dev.script(os.path.join(ROOT, hw_script or script), hw)
    diff = ctx.path('diff.png')
    emu = ctx.path('emu.png')
    subprocess.run(['cp', ref, emu], check=True)
    frac = compare.compare(hw, ref, diff, masks=masks)
    ctx.evidence += ['cases/%s/%s' % (ctx.case_id, n) for n in ('hw.png', 'emu.png', 'diff.png')]
    ctx.note('%.2f%% of pixels differ from the emulator (emulator brightness x%.2f)' % (frac * 100, compare.compare.gain))
    assert frac <= THRESHOLD, 'screen differs from the emulator in %.1f%% of pixels' % (frac * 100)


@case('ui', 'home-edit', covers='#117')
def home_edit(ctx):
    """Jiggle, drag Calendar to the toolbar, make and rename a folder."""
    scripted(ctx, 'tests/ui/t_home_edit.txt', 20)
    since = ctx.state['boot_offset']
    assert not re.search(r'layout.*(fail|error)', ctx.dev.log_text(since), re.I), 'layout save error'


@case('ui', 'home-pages', covers='#117')
def home_pages(ctx):
    """Folder from Videos+Music, drag back out, App Store to a new page."""
    scripted(ctx, 'tests/ui/t_home_pages.txt', 16)


def install_hello(ctx):
    pkg = os.path.join(ROOT, 'out', 'pkg', 'hello.gzp')
    if not os.path.exists(pkg):
        raise Skip('out/pkg/hello.gzp not built (make out/pkg/hello.gzp)')
    ctx.dev.script_text('500 drag 40 280 235 280 500', name='unlock')
    ctx.dev.put(pkg, '/Downloads/hello.gzp')
    ctx.dev.launch('/Downloads/hello.gzp')
    import time
    time.sleep(8)
    ctx.shot('hello-installed')


@case('ui', 'home-installed', covers='#117 #96')
def home_installed(ctx):
    """An installed app joins a folder, then is deleted from inside it."""
    card = os.path.join(ROOT, 'out', 'hw', 'cf-hello.img')
    pkg = os.path.join(ROOT, 'out', 'pkg', 'hello.gzp')
    if not os.path.exists(pkg):
        raise Skip('out/pkg/hello.gzp not built')
    if not os.path.exists(card):
        subprocess.run([os.path.join(ROOT, 'tools', 'mkcfimg.sh'), card, '16', '16', pkg], check=True,
                       capture_output=True)
    # The emulator script stops mid-edit (icons jiggling); end editing in
    # both runs so the final screens are still.
    ref = os.path.join(ROOT, 'out', 'hw', 'scripts', 't_home_installed_home.txt')
    os.makedirs(os.path.dirname(ref), exist_ok=True)
    with open(ref, 'w') as f:
        f.write(open(os.path.join(ROOT, 'tests/ui/t_home_installed.txt')).read().rstrip() + '\n30000 key home\n')
    scripted(ctx, os.path.relpath(ref, ROOT), 36, env={'CF': card},
             hw_script='tests/ui/hw_home_installed.txt', before=install_hello)


@case('ui', 'settings-pages', covers='core')
def settings_pages(ctx):
    """Settings > Date & Time > Time Zone: flings, back to Date & Time."""
    scripted(ctx, 'tests/ui/t_settings_scroll.txt', 18, masks=((0, 0, 240, 16), (0, 40, 240, 140)))
