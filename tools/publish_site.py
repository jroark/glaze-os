#!/usr/bin/env python3
"""Publish glaze-os.com from site/ and check it through the public hostname.
Requires AWS CLI v2; infrastructure is defined in infra/site/stack.json.

HTML is served with no-cache so a release shows at once; everything else may
be cached for an hour. After the upload the whole distribution is invalidated.
"""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path
from urllib.error import HTTPError, URLError
from urllib.request import HTTPRedirectHandler, Request, build_opener

STACK = 'glaze-os-site'
REGION = 'us-east-1'
HTML_CACHE = 'no-cache, max-age=0, must-revalidate'
ASSET_CACHE = 'public, max-age=3600'
EXCLUDE = ['.*', '*/.*']
REQUIRED = ['index.html', '404.html', 'style.css', 'unlock.js', 'favicon.png', 'img/frame.webp',
            'img/lock.png', 'img/knob.png', 'img/label.png', 'img/label-mask.png', 'img/home.png', 'img/og.png']


def aws(profile, *args, capture=True):
    cmd = ['aws', '--region', REGION, *args]
    if profile:
        cmd[1:1] = ['--profile', profile]
    result = subprocess.run(cmd, check=True, text=True, capture_output=capture)
    return result.stdout if capture else ''


def outputs(profile):
    out = aws(profile, 'cloudformation', 'describe-stacks', '--stack-name', STACK,
              '--query', 'Stacks[0].Outputs', '--output', 'json')
    return {o['OutputKey']: o['OutputValue'] for o in json.loads(out)}


def check_site(directory):
    """Fail before any AWS call if the local site is incomplete."""
    for name in REQUIRED:
        if not (directory / name).is_file():
            raise SystemExit(f'site/{name} is missing (images come from tools/mksite.py)')
    release = release_info(directory)
    if release:
        archive = directory / 'downloads' / release['filename']
        if not archive.is_file():
            raise SystemExit(f'{archive} is missing; build it with tools/package_glazeboot.py before publishing')
        data = archive.read_bytes()
        if len(data) != release['size_bytes'] or hashlib.sha256(data).hexdigest() != release['sha256']:
            raise SystemExit('download ZIP does not match downloads/latest.json')
        checksum = directory / 'downloads' / (release['filename'] + '.sha256')
        if not checksum.is_file() or checksum.read_text() != f"{release['sha256']}  {release['filename']}\n":
            raise SystemExit('download checksum file does not match downloads/latest.json')


def release_info(directory):
    path = directory / 'downloads/latest.json'
    if not path.exists():
        return None
    release = json.loads(path.read_text())
    filename = release['filename']
    if Path(filename).name != filename or not filename.endswith('.zip'):
        raise SystemExit('release filename must be a ZIP basename')
    return release


def sync(profile, directory, bucket, dry_run):
    excludes = [arg for pattern in EXCLUDE for arg in ('--exclude', pattern)]
    dry = ['--dryrun'] if dry_run else []
    # Assets first, then the pages that refer to them.
    aws(profile, 's3', 'sync', str(directory), f's3://{bucket}/', '--delete', *excludes,
        '--exclude', '*.html', '--exclude', 'downloads/*', '--cache-control', ASSET_CACHE, *dry, capture=False)
    # Versioned download links must remain valid after the next release.
    # Upload archives and metadata before HTML, without deleting old releases.
    if (directory / 'downloads').is_dir():
        aws(profile, 's3', 'sync', str(directory / 'downloads'), f's3://{bucket}/downloads/',
            *excludes, '--cache-control', ASSET_CACHE, *dry, capture=False)
    aws(profile, 's3', 'sync', str(directory), f's3://{bucket}/', *excludes,
        '--exclude', '*', '--include', '*.html', '--content-type', 'text/html; charset=utf-8',
        '--cache-control', HTML_CACHE, *dry, capture=False)


class NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):
        return None


def fetch(url):
    """Status, headers and body of `url`, without following redirects."""
    request = Request(url, headers={'User-Agent': 'glaze-publish-site'})
    try:
        with build_opener(NoRedirect).open(request, timeout=20) as response:
            return response.status, dict(response.headers), response.read()
    except HTTPError as error:
        return error.code, dict(error.headers), error.read()
    except URLError as error:
        print(f'     {url}: {error.reason}')
        return 0, {}, b''


def verify(site_url, bucket, directory):
    failures = []

    def expect(ok, what):
        print(('ok   ' if ok else 'FAIL ') + what)
        if not ok:
            failures.append(what)

    status, headers, body = fetch(site_url + '/')
    expect(status == 200 and body == (directory / 'index.html').read_bytes(), f'{site_url}/ serves site/index.html')
    expect(headers.get('Cache-Control') == HTML_CACHE, 'index.html is no-cache')
    status, headers, body = fetch(site_url + '/img/frame.webp')
    expect(status == 200 and body == (directory / 'img/frame.webp').read_bytes(), 'img/frame.webp matches')
    status, _, body = fetch(site_url + '/no-such-page')
    expect(status == 404 and body == (directory / '404.html').read_bytes(), 'missing pages get 404.html')
    host = site_url.split('://', 1)[1]
    status, headers, _ = fetch(f'https://www.{host}/img/lock.png')
    expect(status == 301 and headers.get('Location') == f'{site_url}/img/lock.png', 'www redirects to the apex')
    status, headers, _ = fetch(f'http://{host}/')
    expect(status == 301 and headers.get('Location', '').startswith('https://'), 'http redirects to https')
    status, _, _ = fetch(f'https://{bucket}.s3.amazonaws.com/index.html')
    expect(status == 403, 'the S3 origin denies anonymous reads')
    release = release_info(directory)
    if release:
        status, headers, body = fetch(site_url + '/downloads/' + release['filename'])
        expect(status == 200 and len(body) == release['size_bytes'] and
               hashlib.sha256(body).hexdigest() == release['sha256'], 'download ZIP matches the release checksum')
        expect(headers.get('Content-Type', '').split(';')[0] == 'application/zip', 'download is served as a ZIP')
        status, _, body = fetch(site_url + '/downloads/' + release['filename'] + '.sha256')
        expect(status == 200 and body == (directory / 'downloads' / (release['filename'] + '.sha256')).read_bytes(),
               'published SHA-256 checksum matches')
        status, _, body = fetch(site_url + '/downloads/latest.json')
        expect(status == 200 and body == (directory / 'downloads/latest.json').read_bytes(), 'release metadata matches')
    if failures:
        raise SystemExit(f'{len(failures)} check(s) failed; if DNS or the invalidation is still propagating, '
                         'rerun with --verify-only')


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--profile', help='AWS CLI profile (the local default may not be valid)')
    parser.add_argument('--dir', type=Path, default=Path(__file__).resolve().parent.parent / 'site')
    parser.add_argument('--dry-run', action='store_true', help='show what would change; upload nothing')
    parser.add_argument('--verify-only', action='store_true', help='only run the public checks')
    args = parser.parse_args()

    directory = args.dir.resolve()
    check_site(directory)
    out = outputs(args.profile)
    bucket, distribution, site_url = out['BucketName'], out['DistributionId'], out['SiteUrl']
    if not args.verify_only:
        sync(args.profile, directory, bucket, args.dry_run)
        if args.dry_run:
            return
        inval = json.loads(aws(args.profile, 'cloudfront', 'create-invalidation', '--distribution-id',
                               distribution, '--paths', '/*', '--output', 'json'))['Invalidation']['Id']
        print(f'waiting for invalidation {inval}')
        aws(args.profile, 'cloudfront', 'wait', 'invalidation-completed', '--distribution-id', distribution,
            '--id', inval)
    verify(site_url, bucket, directory)


if __name__ == '__main__':
    main()
