#!/usr/bin/env python3
"""Publish verified App Store assets first and atomically replace the catalog last.
Requires AWS CLI v2; infrastructure is defined in infra/appstore/stack.json.
"""
import argparse
import hashlib
import json
import re
import subprocess
import tempfile
from pathlib import Path
from urllib.error import HTTPError
from urllib.request import Request, urlopen

import mkstore

ASSET_CACHE = 'public, max-age=31536000, immutable'
CATALOG_CACHE = 'no-cache, max-age=0, must-revalidate'
FIELDS = set(mkstore.FIELDS) | {'package', 'icon', 'size', 'sha256', 'icon_sha256'}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def artifacts(directory):
    """Validate the entire local release before making any AWS calls."""
    directory = directory.resolve()
    catalog = (directory / 'catalog.txt').read_bytes()
    if not catalog.startswith(b'GZSTORE1\n') or len(catalog) > 65536:
        raise ValueError('Invalid catalog header or size')
    lines = catalog.decode('ascii').splitlines()
    records, record = [], None
    for line in lines[1:]:
        if not line:
            continue
        if line == '[app]':
            record = {}
            records.append(record)
            continue
        if record is None or '=' not in line:
            raise ValueError('Invalid catalog record')
        key, value = line.split('=', 1)
        if key in record or not value or any(not 32 <= ord(c) <= 126 for c in value):
            raise ValueError('Invalid or duplicate catalog field')
        record[key] = value
    if len(records) > 32:
        raise ValueError('Too many apps')
    assets, ids = {}, set()
    for record in records:
        if set(record) != FIELDS or record['id'] in ids:
            raise ValueError('Duplicate app or incomplete record')
        ids.add(record['id'])
        for field, maximum in mkstore.FIELDS.items():
            if len(record[field]) > maximum:
                raise ValueError(f'Catalog {field} too long')
        if not re.fullmatch(r'[a-z0-9][a-z0-9.-]{0,23}', record['id']) or '..' in record['id']:
            raise ValueError('Invalid app ID')
        if not re.fullmatch(r'\d{1,6}(\.\d{1,6})*', record['version']):
            raise ValueError('Invalid app version')
        for field, hash_field in (('package', 'sha256'), ('icon', 'icon_sha256')):
            key = record[field]
            expected = f'apps/{record["id"]}/{record["version"]}/' + ('app.gzp' if field == 'package' else 'icon.gzi')
            if key != expected or not re.fullmatch(r'[a-z0-9./-]+', key):
                raise ValueError('Asset must use its immutable versioned path')
            path = (directory / key).resolve()
            if not path.is_relative_to(directory):
                raise ValueError('Asset escapes release directory')
            data = path.read_bytes()
            if digest(data) != record[hash_field]:
                raise ValueError(f'{key}: checksum mismatch')
            if key in assets:
                raise ValueError('Duplicate asset path')
            assets[key] = path
        data, info, icon = mkstore.package(directory / record['package'])
        if len(data) != int(record['size']) or any(info.get(k) != record[k] for k in ('id', 'name', 'version')):
            raise ValueError('Package metadata or size does not match the catalog')
        if icon != (directory / record['icon']).read_bytes():
            raise ValueError('Catalog icon differs from the package icon')
    return catalog, records, assets


class Aws:
    def __init__(self, profile, region):
        self.prefix = ['aws', '--region', region, '--no-cli-pager', '--output', 'json',
                       '--cli-connect-timeout', '10', '--cli-read-timeout', '60']
        if profile:
            self.prefix += ['--profile', profile]

    def call(self, *args, missing_ok=False):
        result = subprocess.run(self.prefix + list(args), text=True, capture_output=True)
        if result.returncode:
            if missing_ok and re.search(r'\((404|NoSuchKey|NotFound)\)', result.stderr):
                return None
            raise RuntimeError(result.stderr.strip())
        return json.loads(result.stdout) if result.stdout.strip() else {}


def outputs(aws, stack):
    result = aws.call('cloudformation', 'describe-stacks', '--stack-name', stack)['Stacks'][0]
    if result['StackStatus'] not in ('CREATE_COMPLETE', 'UPDATE_COMPLETE'):
        raise RuntimeError(f'Stack is not ready: {result["StackStatus"]}')
    return {item['OutputKey']: item['OutputValue'] for item in result['Outputs']}


def download_object(aws, bucket, key, destination, etag=None):
    args = ['s3api', 'get-object', '--bucket', bucket, '--key', key, '--checksum-mode', 'ENABLED']
    if etag:
        args += ['--if-match', etag]
    return aws.call(*args, str(destination))


def upload(aws, bucket, key, path, cache, etag=None):
    data = path.read_bytes()
    args = ['s3api', 'put-object', '--bucket', bucket, '--key', key, '--body', str(path),
            '--content-type', 'text/plain; charset=us-ascii' if key == 'catalog.txt' else 'application/octet-stream',
            '--cache-control', cache, '--checksum-algorithm', 'SHA256',
            '--metadata', json.dumps({'sha256': digest(data)})]
    args += ['--if-match', etag] if etag else ['--if-none-match', '*']
    return aws.call(*args)


def publish(aws, config, directory, catalog, assets):
    bucket = config['BucketName']
    with tempfile.TemporaryDirectory(prefix='glaze-store-publish-') as tmp:
        downloaded = Path(tmp) / 'object'
        pending = []
        # Check every existing immutable object before uploading new ones.
        for key, path in assets.items():
            head = aws.call('s3api', 'head-object', '--bucket', bucket, '--key', key, missing_ok=True)
            if head is None:
                pending.append((key, path))
            else:
                metadata = download_object(aws, bucket, key, downloaded, head['ETag'])
                if downloaded.read_bytes() != path.read_bytes() or metadata.get('CacheControl') != ASSET_CACHE:
                    raise RuntimeError(f'{key}: immutable remote asset differs; bump the app version')
        for key, path in pending:
            upload(aws, bucket, key, path, ASSET_CACHE)
            metadata = download_object(aws, bucket, key, downloaded)
            if downloaded.read_bytes() != path.read_bytes() or metadata.get('CacheControl') != ASSET_CACHE:
                raise RuntimeError(f'{key}: upload verification failed; catalog was not changed')
            print(f'Uploaded and verified {key}', flush=True)
        head = aws.call('s3api', 'head-object', '--bucket', bucket, '--key', 'catalog.txt', missing_ok=True)
        if head:
            metadata = download_object(aws, bucket, 'catalog.txt', downloaded, head['ETag'])
            if downloaded.read_bytes() == catalog and metadata.get('CacheControl') == CATALOG_CACHE:
                print('Catalog already current.', flush=True)
                return
        upload(aws, bucket, 'catalog.txt', directory / 'catalog.txt', CATALOG_CACHE,
               etag=head['ETag'] if head else None)
        metadata = download_object(aws, bucket, 'catalog.txt', downloaded)
        if downloaded.read_bytes() != catalog or metadata.get('CacheControl') != CATALOG_CACHE:
            raise RuntimeError('Catalog upload verification failed')
        print('Published and verified catalog last.', flush=True)


def verify(config, catalog, assets):
    base = config['StoreUrl']
    for key, expected in [('catalog.txt', catalog)] + [(key, path.read_bytes()) for key, path in assets.items()]:
        request = Request(base + '/' + key, headers={'Accept-Encoding': 'identity'})
        with urlopen(request, timeout=60) as response:
            data = response.read(len(expected) + 1)
            if response.status != 200 or data != expected or response.headers.get('Content-Encoding', 'identity') != 'identity':
                raise RuntimeError(f'{key}: public HTTPS verification failed')
            expected_cache = CATALOG_CACHE if key == 'catalog.txt' else ASSET_CACHE
            if response.headers.get('Cache-Control') != expected_cache:
                raise RuntimeError(f'{key}: wrong cache headers')
        print(f'HTTPS verified {key}', flush=True)
    try:
        with urlopen('https://' + config['BucketOrigin'] + '/catalog.txt', timeout=30):
            raise RuntimeError('S3 origin unexpectedly permits anonymous access')
    except HTTPError as error:
        if error.code != 403:
            raise
    print('Private S3 origin verified (anonymous access denied).', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile', help='AWS CLI profile, e.g. linux4be')
    parser.add_argument('--region', default='us-east-1')
    parser.add_argument('--stack', default='glaze-os-appstore')
    parser.add_argument('--directory', type=Path, default=Path('out/store'))
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--dry-run', action='store_true', help='Validate the release without contacting AWS')
    mode.add_argument('--verify-only', action='store_true', help='Check live HTTPS without changing S3')
    args = parser.parse_args()
    try:
        directory = args.directory.resolve()
        catalog, records, assets = artifacts(directory)
        print(f'Validated {len(records)} apps and {len(assets)} assets.', flush=True)
        if args.dry_run:
            print(json.dumps({'catalog_sha256': digest(catalog), 'assets': list(assets)}, indent=2))
            return
        aws = Aws(args.profile, args.region)
        config = outputs(aws, args.stack)
        if not args.verify_only:
            publish(aws, config, directory, catalog, assets)
        verify(config, catalog, assets)
        print('App Store ready: ' + config['StoreUrl'] + '/catalog.txt', flush=True)
    except (ValueError, KeyError, OSError, RuntimeError) as error:
        parser.exit(1, f'publish_appstore: {error}\n')


if __name__ == '__main__':
    main()
