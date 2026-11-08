"""Automated tests on a real Casio BE-300 over glaze-usb (docs/dev/HWTEST.md).

    python3 -m tools.hwtest run [--suite S ...] [--case REGEX ...] [--resume ID]
    python3 -m tools.hwtest list
"""
import argparse
import os
import sys

from . import runner


def main():
    ap = argparse.ArgumentParser(prog='python3 -m tools.hwtest')
    sub = ap.add_subparsers(dest='cmd', required=True)
    r = sub.add_parser('run', help='run cases on the device')
    r.add_argument('--suite', action='append', choices=runner.SUITES)
    r.add_argument('--case', action='append', help='regex on suite/name')
    r.add_argument('--resume', help='run ID to continue (skips cases that passed)')
    r.add_argument('--host', help='network debugging address instead of USB')
    r.add_argument('--key', default=os.path.expanduser('~/.config/glaze/DEBUG.KEY'))
    r.add_argument('--allow-flash', action='store_true', help='allow cases that write NAND (OTA)')
    r.add_argument('--no-pump', action='store_true', help="don't drain logs in the background")
    sub.add_parser('list', help='list cases')
    a = ap.parse_args()
    if a.cmd == 'list':
        from . import cases  # noqa: F401
        for c in sorted(runner.CASES, key=lambda c: runner.SUITES.index(c['suite'])):
            flag = ' [manual]' if c['manual'] else ' [flash]' if c['destructive'] else ''
            print('%-34s %-14s %s%s' % ('%s/%s' % (c['suite'], c['name']), c['covers'], c['doc'].split('\n')[0], flag))
        return 0
    if not a.host:
        a.key = None
    return runner.run(a)


if __name__ == '__main__':
    sys.exit(main())
