#!/usr/bin/env python3
"""Offline release tests: immutable assets, conditional writes, catalog-last ordering."""
import copy
import hashlib
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import publish_appstore as publisher


class FakeAws:
    def __init__(self):
        self.objects = {}
        self.writes = []
        self.corrupt_download = False

    def call(self, service, operation, *args, missing_ok=False):
        assert service == 's3api'
        key = args[args.index('--key') + 1]
        obj = self.objects.get(key)
        if operation == 'head-object':
            if obj is None:
                assert missing_ok
                return None
            return {'ETag': obj['etag'], 'CacheControl': obj['cache']}
        if operation == 'get-object':
            if '--if-match' in args:
                assert args[args.index('--if-match') + 1] == obj['etag']
            Path(args[-1]).write_bytes(b'corrupt' if self.corrupt_download else obj['body'])
            return {'CacheControl': obj['cache']}
        if operation == 'put-object':
            if '--if-none-match' in args:
                assert args[args.index('--if-none-match') + 1] == '*'
                assert obj is None
            else:
                assert args[args.index('--if-match') + 1] == obj['etag']
            body = Path(args[args.index('--body') + 1]).read_bytes()
            metadata = json.loads(args[args.index('--metadata') + 1])
            assert metadata['sha256'] == publisher.digest(body)
            self.objects[key] = {'body': body, 'cache': args[args.index('--cache-control') + 1],
                                 'etag': '"' + hashlib.sha256(body).hexdigest() + '"'}
            self.writes.append(key)
            return {}
        raise AssertionError(operation)


class Publisher(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = ROOT / 'out/store'
        cls.catalog, cls.records, cls.assets = publisher.artifacts(cls.directory)
        cls.config = {'BucketName': 'fixture'}

    def test_assets_before_catalog_and_idempotent(self):
        aws = FakeAws()
        publisher.publish(aws, self.config, self.directory, self.catalog, self.assets)
        self.assertEqual(aws.writes[-1], 'catalog.txt')
        self.assertEqual(set(aws.writes[:-1]), set(self.assets))
        self.assertEqual(aws.objects['catalog.txt']['body'], self.catalog)
        self.assertEqual(len(aws.writes), 2 * len(self.records) + 1)
        writes = list(aws.writes)
        publisher.publish(aws, self.config, self.directory, self.catalog, self.assets)
        self.assertEqual(writes, aws.writes)

    def test_existing_asset_conflict_preserves_catalog(self):
        aws = FakeAws()
        key = next(iter(self.assets))
        aws.objects[key] = {'body': b'old release', 'cache': publisher.ASSET_CACHE, 'etag': '"old"'}
        aws.objects['catalog.txt'] = {'body': b'old catalog', 'cache': publisher.CATALOG_CACHE, 'etag': '"old-catalog"'}
        before = copy.deepcopy(aws.objects)
        with self.assertRaisesRegex(RuntimeError, 'immutable'):
            publisher.publish(aws, self.config, self.directory, self.catalog, self.assets)
        self.assertEqual(aws.objects, before)
        self.assertEqual(aws.writes, [])

    def test_failed_upload_verification_does_not_publish_catalog(self):
        aws = FakeAws()
        aws.corrupt_download = True
        with self.assertRaisesRegex(RuntimeError, 'upload verification failed'):
            publisher.publish(aws, self.config, self.directory, self.catalog, self.assets)
        self.assertNotIn('catalog.txt', aws.objects)

    def test_updated_catalog_uses_conditional_replace(self):
        aws = FakeAws()
        publisher.publish(aws, self.config, self.directory, self.catalog, self.assets)
        aws.objects['catalog.txt'] = {'body': b'previous catalog', 'cache': publisher.CATALOG_CACHE, 'etag': '"previous"'}
        aws.writes.clear()
        publisher.publish(aws, self.config, self.directory, self.catalog, self.assets)
        self.assertEqual(aws.writes, ['catalog.txt'])

    def test_local_corruption_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp) / 'release'
            shutil.copytree(self.directory, directory)
            key = next(iter(self.assets))
            (directory / key).write_bytes(b'damaged package')
            with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
                publisher.artifacts(directory)

    def test_path_escape_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp) / 'release'
            shutil.copytree(self.directory, directory)
            text = self.catalog.decode().replace('package=apps/hello/1.0/app.gzp', 'package=../credentials')
            (directory / 'catalog.txt').write_text(text)
            with self.assertRaisesRegex(ValueError, 'immutable versioned path'):
                publisher.artifacts(directory)


if __name__ == '__main__':
    unittest.main()
