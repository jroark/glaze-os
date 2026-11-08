"""Case registry, run state and report for hwtest (docs/dev/HWTEST.md)."""
import json
import os
import re
import time
import traceback

from . import images
from .device import Device, DeviceError, ROOT

CASES = []          # registration order is run order within a suite
SUITES = ['preflight', 'ktest', 'boot', 'ui', 'apps', 'hwpkg', 'power', 'usb', 'card', 'ota', 'installer', 'manual']

PASS, FAIL, SKIP, MANUAL, HANG = 'PASS', 'FAIL', 'SKIP', 'MANUAL', 'HANG'


class Skip(Exception):
    pass


class Manual(Exception):
    pass


def case(suite, name, image=None, card=None, covers='', manual=None, destructive=False):
    """Registers a case. image: the RAM image it needs ('ktest', 'default',
    or None for whatever is running). card: card kinds it needs. manual:
    instructions, for a case that needs hands (it is reported, not run)."""
    def wrap(fn):
        CASES.append(dict(suite=suite, name=name, fn=fn, image=image, card=card, covers=covers,
                          manual=manual, destructive=destructive, doc=(fn.__doc__ or '').strip()))
        return fn
    return wrap


# Console lines that fail any case they appear in.
BAD_LINES = re.compile(r'^(PANIC|panic:|kernel: panic|fault: (?!TLB store in ktest_pong).*|.*assertion failed.*)$',
                       re.M)


class Context:
    def __init__(self, dev, rundir, args):
        self.dev, self.rundir, self.args = dev, rundir, args
        self.state = {'image': None, 'card': None}
        self.evidence = []
        self.notes = []

    def log(self, msg):
        line = '[%s] %s' % (time.strftime('%H:%M:%S'), msg)
        print(line, flush=True)
        with open(os.path.join(self.rundir, 'runner.log'), 'a') as f:
            f.write(line + '\n')

    def path(self, name):
        p = os.path.join(self.rundir, 'cases', self.case_id, name)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        return p

    def note(self, msg):
        self.notes.append(msg)
        self.log('  note: ' + msg)

    def shot(self, name):
        p = self.path(name + '.png')
        self.dev.screenshot(p)
        self.evidence.append(os.path.relpath(p, self.rundir))
        return p

    def ensure_image(self, name):
        """Reloads the device into RAM image `name` unless it is running."""
        gb = images.image(name, log=self.log)
        stamp = open(os.path.join(gb, 'hwtest.stamp')).read()
        if self.state.get('image') == stamp and self.dev.probe():
            return
        self.log('reloading image %s' % name)
        t0 = time.time()
        off = self.dev.log_offset()
        try:
            self.dev.reload(gb)
        except DeviceError as e:
            if 'lacks free memory' not in str(e):
                raise
            # Apps and a big RAM disk leave too little for the staged image:
            # boot a small image first, which starts from fresh RAM.
            self.log('not enough memory to reload; bootstrapping through image mini')
            self.dev.reload(images.image('mini', log=self.log))
            off = self.dev.log_offset()
            self.dev.reload(gb)
        self.state['image'] = stamp
        self.state['image_name'] = name
        self.state['boot_offset'] = off
        self.state['reload_s'] = time.time() - t0

    def safety_alarm(self):
        """If the device powers off by itself (idle, a lapsed debugger
        hold-off), nothing but Power wakes it. Arm an alarm 10 minutes out,
        with a 10-minute idle timeout, so a run can't strand it asleep.
        Power cases arm their own alarms over this one."""
        if self.dev.capabilities & 0x200000:
            self.dev.run('power', 'idle', '600', '600000', timeout=30, check=False)

    def check_no_bad_lines(self, since):
        m = BAD_LINES.search(self.dev.log_text(since))
        if m:
            raise AssertionError('console: %s' % m.group(0))


def card_kind(dev):
    """The card in the CF slot, from the console (cf.c) and cf-info."""
    text = dev.log_text()
    kind = 'none'
    for m in re.finditer(r'^(cf: network card (\S+).*|cf: card \S+ .*"(.*)"|cf: network card removed|'
                         r'cf: card removed|gps: .*|cf: ATA .*|card: mounted.*)$', text, re.M):
        line = m.group(0)
        if 'removed' in line:
            kind = 'none'
        elif '2df:8103' in line or 'Marvell' in line:
            kind = 'wl54cf'
        elif 'gps' in line:
            kind = 'gps'
        elif 'ATA' in line or 'mounted' in line:
            kind = 'storage'
        elif 'network card' in line:
            kind = 'nic'
    rc, out = dev.run('cf-info', timeout=20, check=False)
    if rc == 0 and 'No ATA' not in out and 'sectors' in out.lower():
        kind = 'storage'
    return kind


def run(args):
    rid = args.resume or time.strftime('%Y%m%d-%H%M%S')
    rundir = os.path.join(ROOT, 'out', 'hw', 'runs', rid)
    os.makedirs(rundir, exist_ok=True)
    state_path = os.path.join(rundir, 'state.json')
    results = json.load(open(state_path))['results'] if os.path.exists(state_path) else {}
    dev = Device(rundir, host=args.host, key=args.key)
    ctx = Context(dev, rundir, args)
    if os.path.exists(state_path):
        # What the device was left in (NAND boot, installed candidate); the
        # running image is re-checked, not trusted.
        ctx.state.update(json.load(open(state_path)).get('state', {}))
        ctx.state['image'] = None
    from . import cases  # noqa: F401  (registers the cases)

    selected = [c for c in CASES
                if (not args.suite or c['suite'] in args.suite)
                and (not args.case or any(re.search(p, '%s/%s' % (c['suite'], c['name'])) for p in args.case))]
    selected.sort(key=lambda c: SUITES.index(c['suite']))
    ctx.log('run %s: %d cases -> %s' % (rid, len(selected), rundir))
    if not args.no_pump:
        dev.start_pump()
    try:
        for c in selected:
            cid = '%s/%s' % (c['suite'], c['name'])
            if args.resume and cid in results and results[cid]['status'] in (PASS, SKIP, MANUAL) \
                    and not args.case:
                continue
            ctx.case_id = cid.replace('/', '-')
            ctx.evidence, ctx.notes = [], []
            t0 = time.time()
            since = dev.log_offset()
            status, detail = PASS, ''
            ctx.log('== %s' % cid)
            try:
                if c['manual']:
                    raise Manual(c['manual'])
                if c['destructive'] and not args.allow_flash:
                    raise Skip('writes NAND; rerun with --allow-flash')
                if c['image']:
                    ctx.ensure_image(c['image'])
                ctx.safety_alarm()
                if c['card']:
                    kind = ctx.state.get('card') or card_kind(dev)
                    ctx.state['card'] = kind
                    if kind not in c['card']:
                        raise Skip('needs %s card, found %s' % ('/'.join(c['card']), kind))
                c['fn'](ctx)
                ctx.check_no_bad_lines(since)
            except Skip as e:
                status, detail = SKIP, str(e)
            except Manual as e:
                status, detail = MANUAL, str(e)
            except (AssertionError, DeviceError) as e:
                status, detail = FAIL, str(e) or traceback.format_exc(limit=2)
                if isinstance(e, DeviceError) and not dev.probe():
                    status = HANG
                    detail += ' (device not answering)'
            except Exception:
                status, detail = FAIL, traceback.format_exc(limit=4)
            if status in (FAIL, HANG):
                with open(ctx.path('console.log'), 'w') as f:
                    f.write(dev.log_text(since)[-20000:])
                ctx.evidence.append(os.path.relpath(ctx.path('console.log'), rundir))
            results[cid] = dict(status=status, detail=detail, seconds=round(time.time() - t0, 1),
                                covers=c['covers'], evidence=ctx.evidence, notes=ctx.notes,
                                build=dev.build)
            ctx.log('   %s %s' % (status, detail.splitlines()[0] if detail else ''))
            json.dump(dict(results=results, state=ctx.state), open(state_path, 'w'), indent=1)
            if status == HANG:
                if not recover(ctx):
                    ctx.log('device lost; stopping (resume with --resume %s)' % rid)
                    break
    finally:
        dev.stop_pump()
        write_report(rundir, rid, results)
    ctx.log('report: %s' % os.path.join(rundir, 'report.md'))
    return 0 if all(r['status'] in (PASS, SKIP, MANUAL) for r in results.values()) else 1


def recover(ctx):
    """After a hang: bus reset, then a reload of the default image."""
    ctx.log('recovering: reset-link')
    ctx.dev.run('reset-link', timeout=30, check=False)
    try:
        ctx.dev.wait_device(90)
    except DeviceError:
        return False
    ctx.state['image'] = None
    return True


def write_report(rundir, rid, results):
    order = {s: i for i, s in enumerate(SUITES)}
    rows = sorted(results.items(), key=lambda kv: (order.get(kv[0].split('/')[0], 99)))
    counts = {}
    for _, r in rows:
        counts[r['status']] = counts.get(r['status'], 0) + 1
    out = ['# Hardware test run %s' % rid, '',
           ' · '.join('%s %d' % kv for kv in sorted(counts.items())), '',
           '| Case | Status | Covers | Detail | Evidence |', '|---|---|---|---|---|']
    for cid, r in rows:
        detail = (r['detail'] or '').replace('\n', ' ').replace('|', '\\|')[:300]
        notes = '; '.join(r.get('notes') or []).replace('|', '\\|')
        if notes:
            detail = (detail + ' — ' if detail else '') + notes[:300]
        ev = ' '.join('[%s](%s)' % (os.path.basename(e), e) for e in r.get('evidence') or [])
        out.append('| %s | %s | %s | %s | %s |' % (cid, r['status'], r.get('covers', ''), detail, ev))
    with open(os.path.join(rundir, 'report.md'), 'w') as f:
        f.write('\n'.join(out) + '\n')
    json.dump(results, open(os.path.join(rundir, 'results.json'), 'w'), indent=1)
