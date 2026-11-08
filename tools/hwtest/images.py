"""GlazeBoot RAM images for test cases, built once per git state and flags.

Each variant builds in its own OUT directory (out/hw/img-<name>) so variants
don't rebuild each other. A stamp file holds the build ID and flags; an image
whose stamp matches the current `git describe --dirty` is reused.
"""
import hashlib
import os
import subprocess

from .device import ROOT

VARIANTS = {
    # name: make arguments after `glazeboot`
    'ktest': ['INIT=bin/ktest'],
    'default': ['GB_APPS=all'],
    'mini': ['GB_APPS=settings'],     # small enough to reload with little free RAM
}

# glaze.cfg overrides per variant (docs/dev/GLAZEBOOT.md keys).
CFG = {
    'default': {'ramfs_kb': '6656'},
}


def describe():
    try:
        return subprocess.run(['git', 'describe', '--always', '--dirty'], cwd=ROOT, capture_output=True,
                              text=True, check=True).stdout.strip()
    except Exception:
        return 'dev'


def tree_hash():
    """Distinguishes dirty trees: describe alone says only '-dirty'."""
    p = subprocess.run(['git', 'diff', 'HEAD', '--', '.', ':(exclude)tools/hwtest', ':(exclude)tests/ui/hw_*',
                        ':(exclude)docs'], cwd=ROOT, capture_output=True)
    return hashlib.sha1(p.stdout).hexdigest()[:12]


def image(name, extra=(), log=print):
    args = VARIANTS.get(name, []) + list(extra)
    out = os.path.join('out', 'hw', 'img-' + name)
    gb = os.path.join(ROOT, out, 'glazeboot')
    stamp = '%s %s %s %s' % (describe(), tree_hash(), ' '.join(args), sorted(CFG.get(name, {}).items()))
    stamp_path = os.path.join(gb, 'hwtest.stamp')
    try:
        if open(stamp_path).read() == stamp and os.path.exists(os.path.join(gb, 'romfs.img')):
            return gb
    except OSError:
        pass
    log('building image %s (%s)' % (name, ' '.join(args) or 'defaults'))
    logf = os.path.join(ROOT, 'out', 'hw', 'build-%s.log' % name)
    os.makedirs(os.path.dirname(logf), exist_ok=True)
    with open(logf, 'w') as f:
        r = subprocess.run(['./build.sh', 'glazeboot', 'OUT=' + out] + args, cwd=ROOT, stdout=f,
                           stderr=subprocess.STDOUT)
    if r.returncode:
        raise RuntimeError('image %s failed to build; see %s' % (name, logf))
    cfg_path = os.path.join(gb, 'glaze.cfg')
    lines = open(cfg_path).read().splitlines()
    for k, v in CFG.get(name, {}).items():
        lines = [('%s=%s' % (k, v)) if l.startswith(k + '=') else l for l in lines]
    open(cfg_path, 'w').write('\n'.join(lines) + '\n')
    with open(stamp_path, 'w') as f:
        f.write(stamp)
    return gb
