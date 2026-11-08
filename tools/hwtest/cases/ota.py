"""Suite 9: an OTA of a release candidate onto the device's NAND (the only
case that writes NAND; needs --allow-flash), then checks on the NAND boot:
LZ4 romfs from NAND, Hibernate through the SPL, the layout across a reset.

The candidate is built and signed beforehand (docs/dev/HWTEST.md):
    ./build.sh OUT=out/hw/nand-V GLAZE_VERSION=V HIBERNATE_VERIFY=1 MODEM=1 PCCARDS=1
    python3 tools/mkota.py build --kernel out/hw/nand-V/kernel.elf --romfs out/hw/nand-V/romfs.img \\
        --version V --spl-min 3 --out out/hw/ota-V
and named with HWTEST_OTA=V."""
import glob
import os
import re
import secrets
import time

from ..device import ROOT
from ..runner import case, Skip

MAINT = os.path.join(ROOT, 'out', 'pkg', 'maintenance.gzp')


def candidate():
    v = os.environ.get('HWTEST_OTA')
    if not v:
        found = sorted(glob.glob(os.path.join(ROOT, 'out', 'hw', 'ota-*', 'os', 'glaze-*.gzu')))
        if not found:
            raise Skip('no signed candidate in out/hw/ota-*/os (see cases/ota.py)')
        path = found[-1]
        v = re.search(r'glaze-(.+)\.gzu$', path).group(1)
    else:
        path = os.path.join(ROOT, 'out', 'hw', 'ota-' + v, 'os', 'glaze-%s.gzu' % v)
    if not os.path.exists(path):
        raise Skip('%s missing' % path)
    return v, path


def to_nand(ctx):
    """Leaves a RAM boot for the NAND-installed OS (a cold reset through the
    ROM and the SPL); returns the build string it reports."""
    ctx.dev.pump_paused.set()
    try:
        ctx.dev.run('reboot', '--ce', timeout=60, check=False)
        time.sleep(20)
        ctx.dev.wait_device(240)
    finally:
        ctx.dev.pump_paused.clear()
    ctx.state['image'] = None
    ctx.state['nand'] = True
    ctx.dev.drain()
    return ctx.dev.build


def unlock(ctx):
    ctx.dev.script_text('500 drag 40 280 235 280 500', name='unlock')


@case('ota', 'install', covers='#108 #114 OTA', destructive=True)
def install(ctx):
    """Stage the signed candidate through the maintenance package; the SPL
    installs it; the device comes back running it from NAND."""
    version, gzu = candidate()
    since = ctx.dev.log_offset()
    build = to_nand(ctx)
    ctx.note('NAND OS before: %s' % build)
    if not os.path.exists(MAINT):
        raise Skip('out/pkg/maintenance.gzp not built')
    unlock(ctx)
    ctx.dev.put(MAINT, '/Downloads/maintenance.gzp')
    ctx.dev.launch('/Downloads/maintenance.gzp')
    time.sleep(5)
    try:
        ctx.dev.put(gzu, '/Library/Updates/installer.gzu', timeout=900)
    except Exception as e:
        raise AssertionError('could not stage the update (free flash space?): %s' % e)
    rid = secrets.token_hex(16)
    req = ctx.path('request')
    with open(req, 'w') as f:
        f.write('%s update' % rid)
    t0 = time.time()
    ctx.dev.put(req, '/Library/Installer/request')
    # The maintenance app verifies (Ed25519, SHA-256), replies "arming",
    # and the kernel resets into the SPL, which installs and boots it.
    ctx.dev.pump_paused.set()
    try:
        deadline = time.time() + 600
        result = ''
        while time.time() < deadline:
            rc, out = ctx.dev.run('get', '/Library/Installer/result', ctx.path('result'), timeout=30,
                                  retries=0, check=False)
            if rc == 0:
                result = open(ctx.path('result')).read()
                if rid in result and ('arming' in result or '"status":-' in result.replace(' ', '')):
                    break
            elif not ctx.dev.probe():
                break           # already resetting
            time.sleep(3)
        ctx.note('maintenance result: %s' % result.strip()[:200])
        assert 'arming' in result or not ctx.dev.probe(), 'update refused: %s' % result.strip()[:300]
        time.sleep(30)
        ctx.dev.wait_device(2400)   # about 22 minutes on a real BE-300 (207 blocks)
    finally:
        ctx.dev.pump_paused.clear()
    took = time.time() - t0
    ctx.state['boot_offset'] = ctx.dev.log_offset()     # the new OS's boot lines start here
    ctx.dev.drain()
    text = ctx.dev.log_text(since)
    m = re.search(r'^Glaze (\S+) \((\S+)\) booting', text[text.rfind('Glaze '):] if 'Glaze ' in text else '', re.M)
    spl = re.findall(r'^ota: .*$', text, re.M)
    for s in spl:
        ctx.note(s)
    ctx.note('verify to reconnect: %.0f s' % took)
    assert m and m.group(1) == version, 'booted %s, wanted %s' % (m.group(1) if m else '?', version)
    assert any('installed' in s for s in spl), 'no "ota: the SPL installed" line'
    ctx.state['ota_installed'] = version


@case('ota', 'nand-boot-lz4', covers='#114 NAND')
def nand_boot(ctx):
    """The NAND build pages its LZ4 romfs from NAND and reaches SpringBoard."""
    if not ctx.state.get('ota_installed'):
        raise Skip('the candidate is not installed (ota/install did not pass)')
    text = ctx.dev.log_text(ctx.state.get('boot_offset', 0))
    boot = text[text.rfind('booting'):]
    m = re.search(r'^romfs: (.*)$', boot, re.M)
    assert m and 'NAND (LZ4)' in m.group(1), 'romfs: %s' % (m.group(1) if m else 'missing')
    ctx.note('romfs: ' + m.group(1))
    r = re.search(r'^sb: ready (\d+) ms', boot, re.M)
    if r:
        ctx.note('SpringBoard ready %s ms after start' % r.group(1))
    ctx.shot('lock')


@case('ota', 'hibernate-alarm', covers='POWER hibernate')
def hibernate(ctx):
    """Hibernate from NAND, alarm wake through the SPL, every page intact."""
    if not ctx.state.get('ota_installed'):
        raise Skip('the candidate is not installed (ota/install did not pass)')
    from .power import need_power, sleep_and_wait
    need_power(ctx)
    since = ctx.dev.log_offset()
    m, _ = sleep_and_wait(ctx, ['hibernate', '60'], 60, since, extra=120)
    text = ctx.dev.log_text(since)
    assert m.group(1) == 'alarm', 'woke by %s' % m.group(1)
    assert re.search(r'^power: resumed by the SPL', text, re.M), 'no SPL resume (suspended instead?)'
    v = re.search(r'verify: (\d+) pages changed', text)
    if v:
        ctx.note('HIBERNATE_VERIFY: %s pages changed' % v.group(1))
        assert v.group(1) == '0', '%s pages changed across Hibernate' % v.group(1)


@case('ota', 'layout-survives-reset', covers='#117')
def layout(ctx):
    """A home-screen edit saved on NAND is there after a cold reset."""
    if not ctx.state.get('ota_installed'):
        raise Skip('the candidate is not installed (ota/install did not pass)')
    # Leaves a Utilities folder (Clock, Photos) and Calendar in the toolbar on
    # the device's real home screen.
    script = os.path.join(ROOT, 'tests', 'ui', 't_home_edit.txt')
    ctx.dev.script(script)
    ctx.dev.script_text('300 key home\n1500 key home', ctx.path('edited.png'), name='close-folder')
    to_nand(ctx)
    unlock(ctx)
    time.sleep(1)
    after = ctx.shot('after-reset')
    from .. import compare as cmp
    frac = cmp.compare(ctx.path('edited.png'), after, ctx.path('diff.png'))
    ctx.evidence += ['cases/%s/edited.png' % ctx.case_id, 'cases/%s/diff.png' % ctx.case_id]
    ctx.note('%.2f%% of the home screen differs after the reset' % (frac * 100))
    assert frac < 0.02, 'home screen differs after the reset'
