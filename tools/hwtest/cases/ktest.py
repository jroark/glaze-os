"""Suite 1: the kernel self-test, RAM-booted on the device."""
import re

from ..runner import case


@case('ktest', 'ktest', image='ktest', covers='#118 #114 core')
def ktest(ctx):
    """INIT=bin/ktest from a RAM boot: every case PASS or an explained SKIP."""
    since = ctx.state.get('boot_offset', 0)
    ctx.dev.wait_log(r'^ktest: (ALL PASS|FAILED).*$', since=since, timeout=900)
    text = ctx.dev.log_text(since)
    passes = len(re.findall(r'^ktest: PASS', text, re.M))
    skips = re.findall(r'^ktest: SKIP (.*)$', text, re.M)
    infos = re.findall(r'^ktest: INFO (.*)$', text, re.M)
    fails = re.findall(r'^ktest: FAIL (.*)$', text, re.M)
    with open(ctx.path('ktest.log'), 'w') as f:
        f.write('\n'.join(l for l in text.splitlines() if l.startswith('ktest:')) + '\n')
    ctx.evidence.append('cases/%s/ktest.log' % ctx.case_id)
    ctx.note('%d pass, %d skip (%s)' % (passes, len(skips), '; '.join(s.split(' (')[0] for s in skips)))
    for i in infos:
        ctx.note('INFO ' + i)
    assert not fails, 'ktest failures: ' + '; '.join(fails)
    assert passes > 50, 'only %d passes' % passes
