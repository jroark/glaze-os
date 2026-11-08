"""Suite 10: glaze-installer's headless workers against the NAND-booted
device: inventory, install and the (never hardware-tested) app removal.

configure-system and install/restore program NAND through a repair boot,
which this campaign doesn't do (one OTA only); they stay manual."""
import json
import os
import subprocess

from ..device import ROOT
from ..runner import case, Skip

INSTALLER = os.path.join(os.path.dirname(ROOT), 'glaze-installer')
# The macOS build's bundle (the release-* "installed" trees are Windows').
RESOURCES = os.environ.get('HWTEST_INSTALLER_RESOURCES', os.path.join(INSTALLER, 'out', 'installer', 'resources'))
PYTHON = os.path.join(INSTALLER, 'out', 'installer-venv', 'bin', 'python3')
APP = 'package:fifteen'


def worker(ctx, name, apps=None, timeout=900):
    if not os.path.isdir(RESOURCES):
        raise Skip('no installer bundle at %s' % RESOURCES)
    py = PYTHON if os.path.exists(PYTHON) else 'python3'
    argv = [py, os.path.join(INSTALLER, 'installer', 'app.py'), '--resources', RESOURCES, '--worker', name]
    if apps is not None:
        argv += ['--apps', json.dumps(apps)]
    # Glaze launches the companion only while unlocked, as a user would be.
    ctx.dev.script_text('500 drag 40 280 235 280 500', name='unlock')
    # The installer opens USB itself: keep hwtest's own glaze-usb off it.
    ctx.dev.pump_paused.set()
    try:
        with ctx.dev.lock:
            p = subprocess.run(argv, capture_output=True, text=True, timeout=timeout, cwd=INSTALLER)
    finally:
        ctx.dev.pump_paused.clear()
    events = []
    for line in p.stdout.splitlines():
        try:
            events.append(json.loads(line))
        except ValueError:
            pass
    with open(ctx.path('%s.jsonl' % name), 'w') as f:
        f.write(p.stdout + '\n--- stderr ---\n' + p.stderr)
    ctx.evidence.append('cases/%s/%s.jsonl' % (ctx.case_id, name))
    errors = [e for e in events if e.get('event') == 'error']
    assert p.returncode == 0 and not errors, '%s: %s' % (name, (errors[0]['text'] if errors else p.stderr[-300:]))
    return events


def info(events):
    for e in reversed(events):
        if e.get('event') == 'maintenance-info':
            return e
    raise AssertionError('no maintenance-info event')


def need_nand(ctx):
    if not ctx.state.get('nand'):
        raise Skip('needs the NAND boot (run ota/install, or reboot --ce, first)')


@case('installer', 'maintenance-info', covers='installer #2')
def maintenance_info(ctx):
    """Inventory over the maintenance companion (protocol 2)."""
    need_nand(ctx)
    i = info(worker(ctx, 'maintenance-info'))
    ctx.note('protocol %s, OS %s, %d apps' % (i.get('protocol'), i.get('os') or i.get('version'),
                                              len(i.get('apps', {}))))
    assert i.get('protocol', 0) >= 2


@case('installer', 'install-app', covers='installer #2')
def install_app(ctx):
    """Install Fifteen from the installer's bundle."""
    need_nand(ctx)
    events = worker(ctx, 'install-apps', [APP])
    assert any(e.get('event') == 'success' for e in events)
    i = info(worker(ctx, 'maintenance-info'))
    assert 'fifteen' in i.get('apps', {}), 'fifteen not in the inventory after install'


@case('installer', 'uninstall-app', covers='installer #2 #96')
def uninstall_app(ctx):
    """Remove Fifteen and its data (untested on hardware before)."""
    need_nand(ctx)
    events = worker(ctx, 'uninstall-apps', [APP])
    assert any(e.get('event') == 'success' for e in events)
    i = info(events)
    assert 'fifteen' not in i.get('apps', {}), 'fifteen still installed'


@case('installer', 'configure-system', covers='installer #2', manual=(
    'Glaze Installer → Built-in apps / WL54-CF support: reprograms NAND through a repair RAM boot '
    '(~21 min). Outside this campaign\'s one-OTA limit.'))
def configure_system(ctx):
    pass
